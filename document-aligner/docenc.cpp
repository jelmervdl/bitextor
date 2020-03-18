#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include <deque>
#include "util/file_stream.hh"
#include "util/file_piece.hh"
#include "util/string_piece.hh"
#include "src/base64.h"

using namespace std;

enum Mode {
	COMPRESS,
	DECOMPRESS
};

void decode(util::FilePiece &in, util::FileStream &out, char separator, deque<size_t> const &indices) {
	size_t document_index = 0;
	deque<size_t>::const_iterator indices_it(indices.begin());


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
		out << document << separator;

		// Have we found all our indices? Then stop early
		if (!indices.empty() && indices_it == indices.end())
			break;
	}
}

void encode(util::FilePiece &in, util::FileStream &out, char separator, deque<size_t> const &indices) {
	size_t document_index = 0;
	string document;
	deque<size_t>::const_iterator indices_it(indices.begin());

	bool is_eof = false;
	while (!is_eof) {
		document.clear();
		++document_index;

		// Start accumulating lines that make up a document
		StringPiece line;
		while (true) {
			is_eof = !in.ReadLineOrEOF(line, separator, true);
			
			if (is_eof)
				break;

			// Is this the document separator when using \n\n as delimiter?
			if (line.empty())
				break;
			
			document.append(line.data(), line.size());
			
			// Add back the \n separator for lines
			if (separator == '\n')
				document.push_back('\n');

			if (separator != '\n')
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
	cerr << "Usage: " << program_name << " [ -d ] [ -0 ] [ index ... ]\n";
	return 1;
}

int main(int argc, char **argv) {
	deque<size_t> indices;

	Mode mode = COMPRESS;

	char separator = '\n'; // default: second newline

	for (int i = 1; i < argc; ++i) {
		if (argv[i][0] == '-') {
			switch (argv[i][1]) {
				case 'd':
					mode = DECOMPRESS;
					break;

				case '0':
					separator = '\0';
					break;

				default:
					cerr << "Unknown option " << argv[i] << '\n';
					return usage(argv[0]);
			}
		} else {
			size_t index = atoi(argv[i]);
			
			if (index == 0) {
				cerr << "Did not understand document index " << argv[i] << '\n';
				return usage(argv[0]);
			}

			indices.push_back(index);
		}
	}

	sort(indices.begin(), indices.end());

	util::FilePiece in(STDIN_FILENO);
	util::FileStream out(STDOUT_FILENO);

	switch (mode) {
		case DECOMPRESS:
			decode(in, out, separator, indices);
			break;
		case COMPRESS:
			encode(in, out, separator, indices);
			break;
	}

	return 0;
}