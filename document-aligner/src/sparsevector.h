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
	 * Dot-product of two sparse matrices.
	 */
	Scalar dot(SparseVector<Scalar,Index> const &right) const {
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
