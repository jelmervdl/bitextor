#pragma once
#include <algorithm>
#include <climits>
#include <cmath>
#include <vector>
#include <iterator>
#include <immintrin.h>

namespace bitextor {

template <class Scalar, class Index = uint64_t> class SparseVector {
public:
	typedef Scalar value_type;

	typedef typename std::vector<Index>::iterator iterator;

	typedef typename std::vector<Index>::const_iterator const_iterator;

	SparseVector() : fill_(0) {
		//
	}

	/**
	 * Same as operator[], but compatible with Eigen::SparseVector
	 */
	Scalar &insert(Index const &index) {
		return operator[](index);
	}

	Scalar &operator[](Index const &index) {
		auto it = std::lower_bound(begin(), end(), index);
		if (it == end() || *it != index) {
			indices_.insert(it, index);
			return *values_.emplace(values_.begin() + std::distance(begin(), it), fill_);
		} else {
			return *(values_.begin() + std::distance(begin(), it));
		}
	}

	Scalar const &operator[](Index const &index) const {
		auto it = std::lower_bound(begin(), end(), index);
		if (it == end() || *it != index) {
			return fill_;
		} else {
			return *(values_.begin() + std::distance(begin(), it));
		}
	}

	size_t size() const {
		return values_.size();
	}

	void reserve(size_t capacity) {
		indices_.reserve(capacity + (4 - capacity % 4) % 4);
		values_.reserve(capacity);
	}

	void clear() {
		indices_.clear();
		values_.clear();
	}

	iterator begin() {
		return indices_.begin();
	}

	iterator end() {
		return indices_.begin() + size();
	}

	const_iterator cbegin() const {
		return indices_.cbegin();
	}

	const_iterator cend() const {
		return indices_.cbegin() + size();
	}

	template <class T> SparseVector &operator/=(T deliminator) {
		for (auto &value : values_)
			value /= deliminator;
		return *this;
	}

	/**
	 * Dot-product of two sparse vectors.
	 */
	Scalar dot(SparseVector<Scalar,Index> const &right) const {
		// Special case: empty vector means 0.
		if (size() == 0)
			return 0;

		// Assume the right vector is always the larger one
		if (size() > right.size())
			return right.dot(*this);

		// Special case: if the larger vector is much larger than this, use
		// binary search to find the intersection between the two.
		if (right.size() / size() > 10)
			return dot_search(right);
		
		// Otherwise just use the naive but simple & speedy intersection
		return dot_naive(right);
	}

	/**
	 * Just for testing: returns the difference between avx512 and naive impl
	 * to find bugs.
	 */
	Scalar dot_verify_avx512(SparseVector<Scalar,Index> const &right) const {
		return abs(dot_naive(right) - dot_avx512(right));
	}

	__attribute__((__target__("avx512cd")))
	Scalar dot_avx512(SparseVector<Scalar,Index> const &right) const {
		// Assert there is at least something to compare
		if (size() == 0 || right.size() == 0)
			return 0;

		// Make sure our data is nicely chopable in pieces of 4.
		size_t left_padding = (4 - indices_.size() % 4) % 4;
		indices_.resize(indices_.size() + left_padding, ULLONG_MAX);

		size_t right_padding = (4 - right.indices_.size() % 4) % 4;
		right.indices_.resize(right.indices_.size() + right_padding, ULLONG_MAX);

		auto liit = indices_.data(),
		     lend = indices_.data() + size(),
		     riit = right.indices_.data(),
		     rend = right.indices_.data() + right.size();

		auto lvit  = values_.data(),
			 rvit  = right.values_.data();

		float sum = 0;

		__m512i in, out;
		uint64_t buf[8];

		while (liit < lend && riit < rend) {
			// get a chunk of the left & right ngrams in a registry. Left goes in
			// the first four bytes, right in the last four.
			in = _mm512_mask_loadu_epi64(in, 0x0F, liit);
			in = _mm512_mask_loadu_epi64(in, 0xF0, riit - 4); // minus 4 because we want bytes 1-4 at the place of 5-8 (see the 0xF0 mask)

			// Count conflicts. Should only occur in the last four bytes at most
			// since left and right are internally already unique. Conflicts can
			// only occur between the two, not within.
			out = _mm512_conflict_epi64(in);

			// Fetch those bytes from the register
			// TODO: Can we replace this call and the following one with
			// _gather_ plus mask_mul_ somehow?
			_mm512_store_epi64(buf, out);

			// Look at the four bytes and see for each of them whether they say
			// that the value occurred before. I.e. If the value is 8, it was
			// the third doc that is the same. Hence __builtin_ctz (effectively log2).
			for (size_t j = 0; j < 4; ++j) {
				size_t i = __builtin_ctz(buf[4+j]);
				if (buf[4+j] && liit + i < lend && riit + j < rend) // TODO slow?
					sum += lvit[i] * rvit[j];
			}

			// Look at the last word we compared. Is left or right the furthest
			// along in this race? Then take the next chunk for the one that
			// needs to catch up. If we're doing equally well we can just jump
			// ahead on both.
			if (liit[3] < riit[3]) {
				liit += 4;
				lvit += 4;
			} else if (riit[3] < liit[3]) {
				riit += 4;
				rvit += 4;
			} else {
				liit += 4;
				lvit += 4;
				riit += 4;
				rvit += 4;
			}
		}

		return sum;
	}

	/**
	 * dot-product for when right vector is much larger than this vector.
	 */
	Scalar dot_search(SparseVector<Scalar,Index> const &right) const {
		Scalar sum = 0;
	
		auto liit = cbegin(),
			 riit = right.cbegin(),
			 lend = cend(),
			 rend = right.cend();

		while (liit != lend && riit != rend) {
			if (*liit < *riit){
				++liit;
			} else if (*riit < *liit) {
				riit = std::lower_bound(riit, rend, *liit);
			} else {
				sum += values_[std::distance(cbegin(), liit)] * right.values_[std::distance(right.cbegin(), riit)];
				++liit;
				riit = std::lower_bound(riit, rend, *liit);
			}
		}

		return sum;
	}

	/**
	 * dot-product for when both vectors are more or less the same size.
	 */
	Scalar dot_naive(SparseVector<Scalar,Index> const &right) const {
		Scalar sum = 0;
	
		auto liit = cbegin(),
			 riit = right.cbegin(),
			 lend = cend(),
			 rend = right.cend();

		auto lvit = values_.cbegin(),
			 rvit = right.values_.cbegin();

		while (liit != lend && riit != rend) {
			if (*liit < *riit){
				++liit;
				++lvit;
			} else if (*riit < *liit) {
				++riit;
				++rvit;
			} else {
				sum += *lvit * *rvit;
				++liit;
				++lvit;
				++riit;
				++rvit;
			}
		}

		return sum;
	}
private:
	Scalar fill_;
	mutable std::vector<Index> indices_; // mutable because may need to be padded
	std::vector<Scalar> values_;
};

} // namespace
