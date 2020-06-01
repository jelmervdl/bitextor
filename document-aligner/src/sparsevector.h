#pragma once
#include <algorithm>
#include <vector>
#include <iterator>

namespace bitextor {

template <class K, class V> class SparseVector {
public:
	typedef V value_type;

	typedef typename std::vector<K>::iterator iterator;

	typedef typename std::vector<K>::const_iterator const_iterator;

	SparseVector() : fill_(0) {
		//
	}

	void insert(K const &index, V const &value) {
		if (!(indices_.size() == 0 || indices_.back() < index))
			throw std::invalid_argument("indices can only be inserted in incrementing order");

		indices_.push_back(index);
		values_.push_back(value);
	}

	V &operator[](K const &index) {
		auto it = std::lower_bound(indices_.begin(), indices_.end(), index);
		if (it == indices_.end() || *it != index) {
			indices_.insert(it, index);
			return *values_.emplace(values_.begin() + std::distance(indices_.begin(), it), fill_);
		} else {
			return *(values_.begin() + std::distance(indices_.begin(), it));
		}
	}

	V const &operator[](K const &index) const {
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

	/**
	 * Dot-product of two sparse matrices.
	 */
	friend V operator*(SparseVector<K,V> const &left, SparseVector<K,V> const &right) {
		V sum = 0;
	
		auto liit = left.indices_.cbegin(),
			 riit = right.indices_.cbegin(),
			 lend = left.indices_.cend(),
			 rend = right.indices_.cend();

		auto lvit = left.values_.cbegin(),
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
	V fill_;
	std::vector<K> indices_;
	std::vector<V> values_;
};

} // namespace
