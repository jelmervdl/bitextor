#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
#include <vector>
#include "util/file_stream.hh"
#include "util/file_piece.hh"
#include "util/string_piece.hh"
#include "src/base64.h"

using namespace std;

enum Mode {
	COMPRESS,
	DECOMPRESS
};

void decode(util::FilePiece &in, util::FileStream &out, char delimiter, vector<size_t> const &indices) {
	size_t document_index = 0;
	vector<size_t>::const_iterator indices_it(indices.begin());


	for (StringPiece line : in) {
		++document_index;

		if (!indices.empty()) {
			if (*indices_it != document_index) {
				continue; // skip document
			} else {
				indices_it++;
			}
		}

		string document;
		bitextor::base64_decode(line, document);
		out << document << delimiter;

		// Have we found all our indices? Then stop early
		if (!indices.empty() && indices_it == indices.end())
			break;
	}
}

void encode(util::FilePiece &in, util::FileStream &out, char delimiter, vector<size_t> const &indices) {
	size_t document_index = 0;
	string document;
	vector<size_t>::const_iterator indices_it(indices.begin());

	bool is_eof = false;
	while (!is_eof) {
		document.clear();
		++document_index;

		// Start accumulating lines that make up a document
		StringPiece line;
		while (true) {
			is_eof = !in.ReadLineOrEOF(line, delimiter, true);
			
			if (is_eof)
				break;

			// Is this the document delimiter when using \n\n as delimiter?
			if (line.empty())
				break;
			
			document.append(line.data(), line.size());
			
			// Add back the \n delimiter for lines
			if (delimiter == '\n')
				document.push_back('\n');

			if (delimiter != '\n')
				break;
		}

		// Don't bother printing anything for that last empty doc
		if (is_eof && document.empty())
			break;
		
		// Check whether this is a document we care about
		if (!indices.empty()) {
			if (*indices_it != document_index) {
				continue; // skip document
			} else {
				indices_it++;

				// Check whether we can stop processing altogether after this one 
				if (indices_it == indices.end())
					is_eof = true;
			}
		}

		string encoded_document;
		bitextor::base64_encode(StringPiece(document.data(), document.size()), encoded_document);
		out << encoded_document << '\n';
	}
}

int usage(char program_name[]) {
	cerr << "Usage: " << program_name << " [ -d ] [ -0 ] [ index ... ] [ files ... ]\n";
	return 1;
}

bool parse_range(const char *arg, vector<size_t> &indices) {
	stringstream sin(arg);
	
	// Try to read a number
	size_t start;
	if (!(sin >> start))
		return false;

	// Was that all? Done!
	if (sin.peek() == EOF) {
		indices.push_back(start);
		return true;
	}
		
	// See whether we can read the second part of e.g. "1-3"
	size_t end;
	if (sin.get() != '-' || !(sin >> end))
		return false;

	UTIL_THROW_IF(start > end, util::Exception, "Cannot understand " << arg
		<< ": " << start << " is larger than " << end << ".\n");

	// Was that all? Great!
	if (sin.peek() == EOF) {
		while (start <= end)
			indices.push_back(start++);
		return true;
	}

	// There is more, I don't understand
	return false;
}

int main(int argc, char **argv) {
	Mode mode = COMPRESS;

	char delimiter = '\n'; // default: second newline

	vector<util::FilePiece> files;
	vector<size_t> indices;
	
	try {
		for (int i = 1; i < argc; ++i) {
			if (argv[i][0] == '-') {
				switch (argv[i][1]) {
					case 'd':
						mode = DECOMPRESS;
						break;

					case '0':
						delimiter = '\0';
						break;

					default:
						UTIL_THROW(util::Exception, "Unknown option " << argv[i] << ".\n");
				}
			} else if (parse_range(argv[i], indices)) {
				// Okay!
			} else {
				files.emplace_back(argv[i]);
			}
		}
	} catch (util::Exception &e) {
		cerr << e.what();
		return usage(argv[0]);
	}

	sort(indices.begin(), indices.end());

	// If no files are passed in, read from stdi
	if (files.empty())
		files.emplace_back(STDIN_FILENO);

	util::FileStream out(STDOUT_FILENO);

	for (util::FilePiece &in : files) {
		switch (mode) {
			case DECOMPRESS:
				decode(in, out, delimiter, indices);
				break;
			case COMPRESS:
				encode(in, out, delimiter, indices);
				break;
		}
	}

	return 0;
}