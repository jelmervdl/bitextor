#include <algorithm>
#include <exception>
#include <iostream>
#include <memory>
#include <cstdio>
#include <stdexcept>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <thread>
#include <unistd.h>
#include <signal.h>
#include "util/exception.hh"
#include "util/file.hh"
#include "util/pcqueue.hh"
#include "util/file_stream.hh"
#include "util/file_piece.hh"
#include "src/base64.h"


using namespace std;
using namespace bitextor;

/**
 * Single producer queue. Straight up copy of KPU's preprocess library with
 * the addition of an Empty() method that does non-blocking checking whether
 * we might be at the end of the queue.
 */
template <class T> class UnboundedSingleQueue {
  public:
  	typedef util::UnboundedPage<T> Page;

    UnboundedSingleQueue() : valid_(0) {
      SetFilling(new Page());
      SetReading(filling_);
    }

    void Produce(const T &val) {
      if (filling_current_ == filling_end_) {
        Page *next = new Page();
        filling_->next = next;
        SetFilling(next);
      }
      *(filling_current_++) = val;
      valid_.post();
    }

    T& Consume(T &out) {
      util::WaitSemaphore(valid_);
      if (reading_current_ == reading_end_) {
        SetReading(reading_->next);
      }
      out = *(reading_current_++);
      return out;
    }

    // Warning: very much a no-guarantees race-condition-rich implementation!
    // But sufficient for our specific purpose.
    bool Empty() const {
      return reading_current_ == filling_current_;
    }

  private:
    void SetFilling(Page *to) {
      filling_ = to;
      filling_current_ = to->entries;
      filling_end_ = filling_current_ + sizeof(to->entries) / sizeof(T);
    }

    void SetReading(Page *to) {
      reading_.reset(to);
      reading_current_ = to->entries;
      reading_end_ = reading_current_ + sizeof(to->entries) / sizeof(T);
    }

    util::Semaphore valid_;

    Page *filling_;

    std::unique_ptr<Page> reading_;

    T *filling_current_;
    T *filling_end_;
    T *reading_current_;
    T *reading_end_;

    UnboundedSingleQueue(const UnboundedSingleQueue &) = delete;
    UnboundedSingleQueue &operator=(const UnboundedSingleQueue &) = delete;
};


void make_pipe(util::scoped_fd &read_end, util::scoped_fd &write_end) {
	int fds[2];
	
	if (pipe(fds) != 0)
		throw runtime_error("Could not create pipe");

	read_end.reset(fds[0]);
	write_end.reset(fds[1]);
}


class subprocess {
public:
	explicit subprocess(string const &program) : program_(program) {
		//
	}

	void start(char *argv[]) {
		util::scoped_fd process_in;
		util::scoped_fd process_out;

		make_pipe(process_in, in);
		make_pipe(out, process_out);

		pid_ = fork();

		// Are we confused?
		if (pid_ < 0)
			throw runtime_error("Could not fork");

		// Are we the parent?
		if (pid_ > 0)
			// RAII will close process_in and process_out
			return;

		// Terminate if parent stops
		# ifdef __linux__
		prctl(PR_SET_PDEATHSIG, SIGTERM);
		# endif

		// We're in the child, so we don't need these ends of the pipes here.
		in.reset();
		out.reset();

		if (dup2(process_in.get(), STDIN_FILENO) == -1)
			throw runtime_error("dup2 failed on connecting to process stdin");
		else
			process_in.reset();

		if (dup2(process_out.get(), STDOUT_FILENO) == -1)
			throw runtime_error("dup2 failed on connecting to process stdout");
		else
			process_out.reset();

		execvp(program_.c_str(), argv);

		throw util::ErrnoException();
	}

	int wait() {
		int status;

		UTIL_THROW_IF(waitpid(pid_, &status, 0) == -1, util::ErrnoException, "waitpid for child failed");

		if (WIFEXITED(status))
			return WEXITSTATUS(status);
		else
			return 256;
	}

	pid_t pid() const {
		return pid_;
	}

	util::scoped_fd in;
	util::scoped_fd out;
private:
	string program_;
	pid_t pid_;
};


int main(int argc, char **argv) {
	if (argc < 2) {
		cerr << "usage: " << argv[0] << " command [command-args...]\n";
		return 1;
	}

	UnboundedSingleQueue<size_t> line_cnt_queue;

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