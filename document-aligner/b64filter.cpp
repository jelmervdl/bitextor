#include <thread>
#include "util/exception.hh"
#include "util/pcqueue.hh"
#include "util/file_stream.hh"
#include "util/file_piece.hh"
#include "src/single_producer_queue.h"
#include "src/subprocess.h"
#include "src/base64.h"

using namespace std;
using namespace bitextor;

int main(int argc, char **argv) {
	if (argc < 2) {
		cerr << "usage: " << argv[0] << " command [command-args...]\n";
		return 1;
	}

	SingleProducerQueue<size_t> line_cnt_queue;

	subprocess child(argv[1]);

	child.start(argv + 1);

	thread feeder([&child, &line_cnt_queue]() {
		util::FilePiece in(STDIN_FILENO);
		util::FileStream child_in(child.in.get());

		// Decoded document buffer
		string doc;

		for (StringPiece line : in) {
			base64_decode(line, doc);

			// Make the the document ends with a line ending. This to make sure
			// the next doc we send to the child will be on its own line and the
			// line_cnt we do is correct.
			if (doc.back() != '\n')
				doc.push_back('\n');

			size_t line_cnt = count(doc.cbegin(), doc.cend(), '\n');
			
			// Send line count first to the reader, so it can start reading as
			// soon as we start feeding the document to the child.
			line_cnt_queue.Produce(line_cnt);

			// Feed the document to the child.
			// Might block because it can cause a flush.
			child_in << doc;
		}

		// Tell the reader to stop
		line_cnt_queue.Produce(0);

		// Flush (blocks) & close the child's stdin
		child_in.flush();
		child.in.reset();
	});

	thread reader([&child, &line_cnt_queue]() {
		util::FileStream out(STDOUT_FILENO);
		util::FilePiece child_out(child.out.release());

		size_t line_cnt;
		string doc;

		while (line_cnt_queue.Consume(line_cnt) > 0) {
			doc.clear();
			doc.reserve(line_cnt * 4096); // 4096 is not a typical line length

			try {
				while (line_cnt-- > 0) {
					StringPiece line(child_out.ReadLine());
					doc.append(line.data(), line.length());
					doc.push_back('\n');
				}
			} catch (util::EndOfFileException &e) {
				UTIL_THROW(util::Exception, "Sub-process stopped producing while expecting more lines");
			}

			string encoded_doc;
			base64_encode(doc, encoded_doc);
			out << encoded_doc << '\n';

			// Just to check, next time we call Consume(), will we block? If so,
			// that means we've caught up with the producer. However, the order
			// the producer fills line_cnt_queue is first giving us a new line-
			// count and then sending the input to the sub-process. So if we do
			// not have a new line count yet, the sub-process definitely can't
			// have new output yet, and peek should block and once it unblocks
			// we expect to have that line-count waiting. If we still don't,
			// then what is this output that is being produced by the sub-
			// process?
			if (line_cnt_queue.Empty()) {
				// If peek throws EOF now our sub-process stopped before its
				// stdin was closed (producer produces the poison before it
				// closes the sub-process's stdin.)
				child_out.peek();
				
				// peek() came back. We have a line-number now, right? If not
				// sub-process is producing output without any input to base it
				// on. Which is bad.
				if (line_cnt_queue.Empty())
					UTIL_THROW(util::Exception, "sub-process is producing more output than it was given input");
			}
		}
	});

	int retval = child.wait();

	feeder.join();
	reader.join();
	
	return retval;
}