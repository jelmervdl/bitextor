#pragma once
#include <cstdio>
#include <exception>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "util/exception.hh"
#include "util/file.hh"
#include "util/file_stream.hh"

namespace {

void make_pipe(util::scoped_fd &read_end, util::scoped_fd &write_end) {
	int fds[2];
	
	if (pipe(fds) != 0)
		throw std::runtime_error("Could not create pipe");

	read_end.reset(fds[0]);
	write_end.reset(fds[1]);
}

} // namespace

namespace bitextor {

class subprocess {
public:
	explicit subprocess(std::string const &program) : program_(program) {
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
			throw std::runtime_error("Could not fork");

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
			throw std::runtime_error("dup2 failed on connecting to process stdin");
		else
			process_in.reset();

		if (dup2(process_out.get(), STDOUT_FILENO) == -1)
			throw std::runtime_error("dup2 failed on connecting to process stdout");
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
	std::string program_;
	pid_t pid_;
};

} // end namespace