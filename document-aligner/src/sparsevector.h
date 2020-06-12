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

	__attribute__((__target__("avx512cd,avx512vl")))
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

		__m512i in;

		while (liit < lend && riit < rend) {
			// get a chunk of the left & right ngrams in a registry. Left goes in
			// the first four bytes, right in the last four.
			in = _mm512_mask_loadu_epi64(in, 0x0F, liit);
			in = _mm512_mask_loadu_epi64(in, 0xF0, riit - 4); // minus 4 because we want bytes 1-4 at the place of 5-8 (see the 0xF0 mask)

			// Count conflicts. Should only occur in the last four bytes at most
			// since left and right are internally already unique. Conflicts can
			// only occur between the two, not within.
			__m512i conflicts = _mm512_conflict_epi64(in);

			// Now for each of those conflicts figure out what they are conflicting
			// with between left and right. This will give us indices.
			__m512i all_offsets = _mm512_sub_epi64(_mm512_set1_epi64(63), _mm512_lzcnt_epi64(conflicts));

			// We only need the last four positions. There can only be conflicts
			// between the first four and last four.
			__m256i offsets = _mm512_extracti64x4_epi64(all_offsets, 1);

			// Figure out which offsets are bogus because there was no conflict.
			__m256i mask = _mm256_cmpeq_epi64(offsets, _mm256_set1_epi64x(-1));
			__m256i hits = _mm256_xor_si256(mask, _mm256_set1_epi64x(-1));

			// Mask out the conflicts that were caused by padding matching other
			// padding or internal padding.
			auto valid = std::min(lend - liit - left_padding, rend - riit - right_padding);
			hits = _mm256_and_si256(hits, _mm256_set_epi64x(
				valid > 3 ? -1 : 0,
				valid > 2 ? -1 : 0,
				valid > 1 ? -1 : 0,
				valid > 0 ? -1 : 0));

			// Hits is __m256i of four 0xFF or 0x00 uint64_t's. I need uint32_t.
			// for the _mm_blendv_ps. Use permute to cast our 64 bits to 32.
			__m128 hits_i32 = _mm256_extractf128_ps(_mm256_permutevar8x32_epi32(hits, _mm256_set_epi32(7, 5, 3, 1, 6, 4, 2, 0)), 0);

			// Mask out the offsets that are not going to be used so we don't gather
			// from infinitely far away, but just from the base offset.
			offsets = _mm256_and_si256(offsets, hits);
			
			// Gather the values from left (based on the offsets) and put them in 
			// the vector at the positions according to hits. We will be gathering
			// a bit too much but later on set the indices that are not in hits 
			// to 0.
			__m128 vals_lft = _mm256_i64gather_ps(lvit, offsets, 4);

			// Mask out the values that cannot be hits, i.e. make them 0.
			// vals_lft = _mm_blendv_ps(_mm_setzero_ps(), vals_lft, hits_i32);
			vals_lft = _mm_and_ps(vals_lft, hits_i32);
			
			// Indiscriminately load in right values. The 0s in left will cause the
			// values we don't need to turn to 0 in the product.
			__m128 vals_rgt = _mm_loadu_ps(rvit);
			
			// Dot product. Done.
			__m128 dot_prod = _mm_dp_ps(vals_lft, vals_rgt, 0b11111111);

			// Load in the output of the dot product (float broadcast to all
			// four positions in the vector) and pick one to add to our sum.
			float dot_prod_buf[4];
			_mm_store_ps(dot_prod_buf, dot_prod);
			sum += dot_prod_buf[0];

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
