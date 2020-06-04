#pragma once
#include <algorithm>
#include <vector>
#include <iterator>

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
		auto it = std::lower_bound(indices_.begin(), indices_.end(), index);
		if (it == indices_.end() || *it != index) {
			indices_.insert(it, index);
			return *values_.emplace(values_.begin() + std::distance(indices_.begin(), it), fill_);
		} else {
			return *(values_.begin() + std::distance(indices_.begin(), it));
		}
	}

	Scalar const &operator[](Index const &index) const {
		auto it = std::lower_bound(indices_.begin(), indices_.end(), index);
		if (it == indices_.end() || *it != index) {
			return fill_;
		} else {
			return *(values_.begin() + std::distance(indices_.begin(), it));
		}
	}

	size_t size() const {
		return indices_.size();
	}

	void reserve(size_t capacity) {
		indices_.reserve(capacity);
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
		return indices_.end();
	}

	const_iterator cbegin() {
		return indices_.cbegin();
	}

	const_iterator cend() {
		return indices_.cend();
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
	 * dot-product for when right vector is much larger than this vector.
	 */
	Scalar dot_search(SparseVector<Scalar,Index> const &right) const {
		Scalar sum = 0;
	
		auto liit = indices_.cbegin(),
			 riit = right.indices_.cbegin(),
			 lend = indices_.cend(),
			 rend = right.indices_.cend();

		while (liit != lend && riit != rend) {
			if (*liit < *riit){
				++liit;
			} else if (*riit < *liit) {
				riit = std::lower_bound(riit, rend, *liit);
			} else {
				sum += values_[std::distance(indices_.cbegin(), liit)] * right.values_[std::distance(right.indices_.cbegin(), riit)];
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
	
		auto liit = indices_.cbegin(),
			 riit = right.indices_.cbegin(),
			 lend = indices_.cend(),
			 rend = right.indices_.cend();

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
	std::vector<Index> indices_;
	std::vector<Scalar> values_;
};

} // namespace
