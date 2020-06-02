#include "ngram.h"
#include "MurmurHash3.h"
#include <sstream>
#include <iostream>

using namespace std;

namespace {
	inline uint32_t MurmurHashCombine(uint32_t left, uint32_t right) {
		return left ^ (right + 0x9e3779b9 + (left<<6) + (left>>2));
	}
}

namespace bitextor {

NGramIter::NGramIter() {
	//
}

NGramIter::NGramIter(StringPiece const &source, size_t ngram_size)
: token_it_(source, " \n"), // Break on newline as well; I don't care about line beginnings and endings right now
  ngram_size_(ngram_size),
  pos_(0),
  end_(false),
  buffer_(ngram_size) {
	init();
}

void NGramIter::init() {
	for (pos_ = 0; pos_ < ngram_size_ - 1 && token_it_; ++pos_, ++token_it_)
		MurmurHash3_x86_32(token_it_->data(), token_it_->size(), 0, &buffer_[pos_]);
	
	// Some documents are just too short
	if (!token_it_)
		end_ = true;

	increment();
}

void NGramIter::increment() {
	if (!token_it_) {
		end_ = true;
		return;
	}
	
	// Read next word & store hash
	MurmurHash3_x86_32(token_it_->data(), token_it_->size(), 0, &buffer_[pos_ % ngram_size_]);
	
	// Create hash from combining past N word hashes
	ngram_hash_ = 0;
	for (long offset = ngram_size_ - 1; offset >= 0; --offset)
		ngram_hash_ = MurmurHashCombine(buffer_[(pos_ - offset) % ngram_size_], ngram_hash_);
	
	++pos_;
	++token_it_;
}

} // namespace bitextor
