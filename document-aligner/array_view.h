#pragma once
#include <algorithm>

namespace bitextor {

/**
 * Think of this as a wrapper that behaves a bit like the std::list and std::vector containers, but around an
 * already allocated array. It does some limited bounds checking (i.e. [] operator does, but you can walk
 * a pointer or iterator beyond end without any trouble...) Also it behaves a bit like unique_ptr once
 * constructed to make sure you know what you are doing. It can take ownership of the pointer you give it and
 * free it once it is done with it.
 */
template <typename T> class ArrayView {
public:
	ArrayView() : begin_(nullptr), end_(nullptr) {
		//
	}

	ArrayView(T *begin, T *end) : begin_(begin), end_(end), is_owner_(false) {
		//
	}

	ArrayView(ArrayView<T> const &) = delete;

	ArrayView(ArrayView<T> &&other) {
		ArrayView<T>();
		swap(other);
	}

	~ArrayView() {
		if (is_owner_)
			delete[] begin_;
	}

	ArrayView<T> &operator=(ArrayView<T> const &) = delete;

	ArrayView<T> &operator=(ArrayView<T> &&other) {
		swap(other);
		return *this;
	}

	typedef T* iterator;

	typedef T const * const_iterator;

	inline iterator begin() {
		return begin_;
	}

	inline iterator end() {
		return end_;
	}

	inline const_iterator begin() const {
		return const_cast<const T *>(begin_);
	}

	inline const_iterator end() const {
		return const_cast<const T *>(end_);
	}

	inline const_iterator cbegin() const {
		return const_cast<const T *>(begin_);
	}

	inline const_iterator cend() const {
		return const_cast<const T *>(end_);
	}

	inline T &operator[](size_t index) {
		if (begin_ + index >= end_)
			throw std::out_of_range("ArrayView index out of range");

		return begin_[index];
	}

	inline T const &operator[](size_t index) const {
		if (begin_ + index >= end_)
			throw std::out_of_range("ArrayView index out of range");

		return const_cast<const T>(begin_[index]);
	}

	inline size_t size() const {
		return cend() - cbegin();
	}

	static ArrayView<T> allocate(size_t size) {
		T *data = new T[size];
		ArrayView<T> view(data, data + size);
		view.is_owner_ = true;
		return view;
	}

private:
	T *begin_;
	T *end_;
	bool is_owner_;

	void swap(ArrayView<T> &other) {
		std::swap(begin_, other.begin_);
		std::swap(end_, other.end_);
		std::swap(is_owner_, other.is_owner_);
	}
};

} // namespace bitextor
