#pragma once
#include "util/string_piece.hh"
#include "ngram.h"
#include <Eigen/Sparse>
// #include "sparsevector.h"
#include <limits>
#include <unordered_map>

namespace bitextor {

struct Document {
	// Document offset, used as identifier
	size_t id;
	
	// ngram frequency in document
	std::unordered_map<NGram, size_t> vocab;
};

struct DocumentRef {
	// Document offset, used as identifier
	size_t id;
	
	// ngram scores as a sorted array for quick sparse dot product
	Eigen::SparseVector<float, 0, int64_t> wordvec;
	// SparseVector<float, NGram> wordvec;

	DocumentRef() : wordvec(std::numeric_limits<NGram>::max()) {
		//
	}
};

// Assumes base64 encoded still.
void ReadDocument(const StringPiece &encoded, Document &to, size_t ngram_size);

void calculate_tfidf(Document const &document, DocumentRef &document_ref, size_t document_count, std::unordered_map<NGram, size_t> const &df);

float calculate_alignment(DocumentRef const &left, DocumentRef const &right);

} // namespace bitextor
