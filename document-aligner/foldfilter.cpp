#include <thread>
#include <deque>
#include <climits>
#include <type_traits>
#include "util/exception.hh"
#include "util/pcqueue.hh"
#include "util/file_stream.hh"
#include "util/file_piece.hh"
#include "util/utf8.hh"
#include "src/single_producer_queue.h"
#include "src/subprocess.h"

using namespace std;
using namespace bitextor;

// Order determines preference: the first one of these to occur in the
// line will determine the wrapping point.
static UChar32 delimiters[]{':', ',', ' ', '-', '.'};

// is_delimiter functions as lookup table for checking quickly whether a char
// is a delimiter, and if so, at what position it lives. uchar because that
// index can never really high.
size_t is_delimiter(UChar32 character) {
	for (size_t i = 0; i < extent<decltype(delimiters)>::value; ++i)
		if (character == delimiters[i])
			return i + 1;

	return false;
}

pair<deque<StringPiece>,deque<string>> wrap_lines(StringPiece const &line, size_t column_width) {
	deque<StringPiece> out_lines;

	deque<string> out_delimiters;

	// Current byte position
	int32_t pos = 0;

	// Length of line in bytes
	int32_t length = line.size();
	
	// Byte position of last cut-off point
	int32_t pos_last_cut = 0;

	// For each delimiter the byte position of its last occurrence
	int32_t pos_delimiter[extent<decltype(delimiters)>::value]{0};

	// Position of the first delimiter we encountered up to pos. Reset
	// to pos + next char if it's not a delimiter.
	int32_t pos_first_delimiter;

	while (pos < length) {
		UChar32 character;

		U8_NEXT(line.data(), pos, length, character);
		
		if (character < 0)
			throw utf8::NotUTF8Exception(line);

		if (size_t delimiter_idx = is_delimiter(character)) {
			// Store pos_first_delimiter instead of pos because when we have
			// consecutive delimiters we want to chop em all off, even when
			// our ideal delimiter is somewhere in the middle.
			pos_delimiter[delimiter_idx - 1] = pos_first_delimiter;
		} else {
			// Maybe the next char is a delimiter? pos is pointing to the next
			// one right now, U8_NEXT incremented it.
			pos_first_delimiter = pos;
		}

		// Do we need to introduce a break?
		if (pos - pos_last_cut < static_cast<int32_t>(column_width))
			continue;

		// Last resort if we didn't break on a delimiter: just chop where we are
		int32_t pos_cut = pos;

		// Find a more ideal break point by looking back for a delimiter
		for (size_t i = 0; i < extent<decltype(delimiters)>::value; ++i) {
			if (pos_delimiter[i] > pos_last_cut) {
				pos_cut = pos_delimiter[i];
				break;
			}
		}

		// Assume we cut without delimiters (i.e. the last resort scenario)
		int32_t pos_cut_end = pos_cut;

		// Peek ahead to were after the cut we encounter our first not-a-delimiter
		// because that's the point were we resume.
		while (pos_cut_end < length) {
			U8_NEXT(line.data(), pos_cut_end, length, character);
			if (character < 0)
				throw utf8::NotUTF8Exception(line);
			
			if (!is_delimiter(character))
				break;
		}

		out_lines.push_back(line.substr(pos_last_cut, pos_cut - pos_last_cut));
		out_delimiters.emplace_back(line.substr(pos_cut, pos_cut_end - pos_cut).data(), pos_cut_end - pos_cut);
		pos_last_cut = pos_cut_end;
		pos = pos_cut_end;
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
			if (static_cast<size_t>(sentence.size()) > column_width)
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

		for (size_t sentence_num = 1; queue.Consume(delimiters).size() > 0; ++sentence_num) {
			sentence.clear();
			
			// Let's assume that the wrapped process plus the chopped off
			// delimiters won't be more than twice the input we give it.
			sentence.reserve(delimiters.size() * 2 * column_width);

			try {
				while (!delimiters.empty()) {
					StringPiece line(child_out.ReadLine());
					sentence.append(line.data(), line.length());
					sentence.append(delimiters.front());
					delimiters.pop_front();
				}
			} catch (util::EndOfFileException &e) {
				UTIL_THROW(util::Exception, "Sub-process stopped producing while expecting more lines for sentence " << sentence_num << ".");
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

	// Order here doesn't matter that much. If either of the threads is blocked
	// while the other finishes, it's an error state and the finishing thread
	// will finish with an uncaught exception, which will just terminate()
	// everything.
	feeder.join();
	reader.join();
	
	return retval;
}
