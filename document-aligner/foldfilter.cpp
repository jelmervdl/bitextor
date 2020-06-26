#include <thread>
#include <deque>
#include "util/exception.hh"
#include "util/pcqueue.hh"
#include "util/file_stream.hh"
#include "util/file_piece.hh"
#include "src/single_producer_queue.h"
#include "src/subprocess.h"

using namespace std;
using namespace bitextor;

// Order determines preference: the first one of these to occur in the
// line will determine the wrapping point.
static char delimiters[]{':', ',', ' ', '-', '.'};

// is_delimiter functions as lookup table for checking quickly whether a char
// is a delimiter, and if so, at what position it lives. uchar because that
// index can never really high.
static unsigned char is_delimiter[UCHAR_MAX + 1]{0};

pair<deque<StringPiece>,deque<string>> wrap_lines(StringPiece const &line, size_t column_width) {
	deque<StringPiece> out_lines;

	deque<string> out_delimiters;

	size_t pos_last_cut = 0;

	size_t pos = 0;
	
	size_t pos_delimiter[sizeof(delimiters) / sizeof(char)]{0};

	for (; pos < line.size(); ++pos) {
		if (unsigned char delimiter_idx = is_delimiter[static_cast<unsigned char>(line.data()[pos])])
			pos_delimiter[delimiter_idx - 1] = pos;

		// Do we need to introduce a break?
		if (pos - pos_last_cut < column_width)
			continue;

		// Last resort if we didn't break on a delimiter: just chop where we are
		size_t pos_cut = pos;

		for (size_t i = 0; i < sizeof(delimiters) / sizeof(char); ++i) {
			if (pos_delimiter[i] > pos_last_cut) {
				pos_cut = pos_delimiter[i];
				break;
			}
		}

		size_t pos_cut_end = pos_cut + 1;

		// Peek ahead to were after the cut we encounter our first not-a-delimiter
		// because that's the real point were we resume.
		while (pos_cut_end < line.size() && is_delimiter[static_cast<unsigned char>(line.data()[pos_cut_end])] != 0)
			++pos_cut_end;

		out_lines.push_back(line.substr(pos_last_cut, pos_cut - pos_last_cut));
		out_delimiters.emplace_back(line.substr(pos_cut, pos_cut_end - pos_cut).data(), pos_cut_end - pos_cut);
		pos_last_cut = pos_cut_end;
	}

	// Push out any trailing bits
	if (pos_last_cut < pos) {
		out_lines.push_back(line.substr(pos_last_cut, pos - pos_last_cut));
		out_delimiters.push_back("");
	}

	return make_pair(out_lines, out_delimiters);
}

int main(int argc, char **argv) {
	char *program_name = argv[0];

	size_t column_width = 40;

	if (argc > 2 && string(argv[1]) == "-w") {
		column_width = atoi(argv[2]);
		argv += 2;
		argc -= 2;
	}

	if (argc < 2) {
		cerr << "usage: " << program_name << " [-w width] command [command-args ...]\n";
		return 1;
	}

	for (size_t i = 0; i < sizeof(delimiters) / sizeof(char); ++i)
		is_delimiter[static_cast<unsigned char>(delimiters[i])] = i + 1; // plus one because 0 = false = no delimiter

	SingleProducerQueue<deque<string>> queue;

	subprocess child(argv[1]);

	child.start(argv + 1);

	thread feeder([&child, &queue, column_width]() {
		util::FilePiece in(STDIN_FILENO);
		util::FileStream child_in(child.in.get());

		for (StringPiece sentence : in) {
			// Initialize with a single line. Because even if our sentence is
			// empty it is still a line that needs to go through the whole
			// thing.
			deque<StringPiece> lines{sentence};
			deque<string> delimiters{""};

			// If there is anything that needs chopping, let's go chopping.
			if (sentence.size() > column_width)
				tie(lines, delimiters) = wrap_lines(sentence, column_width);

			// Tell the reader that there will be N lines to read to reconstruct
			// this sentence.
			queue.Produce(delimiters);

			// Feed the document to the child.
			// Might block because it can cause a flush.
			for (auto const &line : lines)
				child_in << line << '\n';
		}

		// Tell the reader to stop
		queue.Produce(deque<string>());

		// Flush (blocks) & close the child's stdin
		child_in.flush();
		child.in.reset();
	});

	thread reader([&child, &queue, column_width]() {
		util::FileStream out(STDOUT_FILENO);
		util::FilePiece child_out(child.out.release());

		deque<string> delimiters;
		string sentence;

		while (queue.Consume(delimiters).size() > 0) {
			sentence.clear();
			
			// Let's assume that the wrapped process plus the chopped off
			// delimiters won't be more than twice the input we give it.
			sentence.reserve(delimiters.size() * 2 * column_width);

			try {
				while (delimiters.size() > 0) {
					StringPiece line(child_out.ReadLine());
					sentence.append(line.data(), line.length());
					sentence.append(delimiters.front());
					delimiters.pop_front();
				}
			} catch (util::EndOfFileException &e) {
				UTIL_THROW(util::Exception, "Sub-process stopped producing while expecting more lines");
			}

			// Yes, this might introduce a newline at the end of the file, but
			// yes that is what we generally want in our pipeline because we
			// might concatenate all these files and that will mess up if they
			// don't have a trailing newline.
			out << sentence << '\n';

			// Just to check, next time we call Consume(), will we block? If so,
			// that means we've caught up with the producer. However, the order
			// the producer fills queue is first giving us a new line-
			// count and then sending the input to the sub-process. So if we do
			// not have a new line count yet, the sub-process definitely can't
			// have new output yet, and peek should block and once it unblocks
			// we expect to have that line-count waiting. If we still don't,
			// then what is this output that is being produced by the sub-
			// process?
			if (queue.Empty()) {
				// If peek throws EOF now our sub-process stopped before its
				// stdin was closed (producer produces the poison before it
				// closes the sub-process's stdin.)
				child_out.peek();
				
				// peek() came back. We have a line-number now, right? If not
				// sub-process is producing output without any input to base it
				// on. Which is bad.
				if (queue.Empty())
					UTIL_THROW(util::Exception, "sub-process is producing more output than it was given input");
			}
		}
	});

	int retval = child.wait();

	feeder.join();
	reader.join();
	
	return retval;
}