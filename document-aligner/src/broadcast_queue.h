#pragma once
#include <memory>
#include <mutex>
#include <condition_variable>
#include <stdexcept>
#include <cassert>
#include <iostream>

namespace bitextor {

template <typename T, size_t N = 4096> class BroadcastQueue
{
private:
    static const size_t QUEUE_PAGE_SIZE = N;

    struct Page {
		T data[QUEUE_PAGE_SIZE];
		std::shared_ptr<Page> next;
		inline T* begin() { return data; }
		inline T* end() { return data + QUEUE_PAGE_SIZE; }
	};

	struct Queue {
		std::shared_ptr<Page> page;
		
		T* write_pos;
		size_t value_cnt;

		bool dead;

		std::mutex mutex;
		std::condition_variable added;

		Queue()
		: page(new Page{}),
		  write_pos(page->data),
		  value_cnt(0) {
			//
		}

		/**
		 * Copy value into queue. Might lock briefly to increment value_cnt.
		 */
		void push(T const &value) {
			*write_pos = value;
			
			// Will the next write be on a new page? If so, allocate it before
			// incrementing value_cnt as as soon as we do that, all the listeners
			// might jump to it while incrementing their internal offset & pos.
			if (++write_pos == page->end()) {
				page->next = std::shared_ptr<Page>(new Page{});
				page = page->next;
				write_pos = page->begin();
			}

			// Tell our listeners that there is a new value available.
			{
				std::unique_lock<std::mutex> lock(mutex);
				++value_cnt;
				added.notify_all();
			}
		}

		/**
		 * Wait for certain value to become available. If there is 1 value
		 * available, offset 0 will return immediately, offset 1 will block.
		 */
		void wait_for(size_t offset) {
			// Happy path
			if (value_cnt > offset)
				return;

			// Slow path
			std::unique_lock<std::mutex> lock(mutex);
			while (value_cnt <= offset)
				added.wait(lock);
		}
	};
	
	class Listener {
	public:
		Listener()
		: queue_(nullptr),
		  page_(nullptr),
		  offset_(0),
		  pos_(0)
		{
		  	//
		}

		explicit Listener(std::shared_ptr<Queue> queue)
		: queue_(queue),
		  page_(queue_->page),
		  offset_(queue_->value_cnt),
		  pos_(page_->data + (offset_ % QUEUE_PAGE_SIZE))
		{
			//
		}

		T &pop(T &message) {
			if (!queue_)
				throw std::logic_error("calling pop() on unconnected listener");

			// Wait till there are enough messages
			queue_->wait_for(offset_);

			// Consume the value
			message = *pos_;
			
			// Increment our internal offset
			++offset_;

			// And increment our internal page pointer. If we've reached the end
			// of the page we can jump to the next. Produce() guarantees it is
			// already available.
			if (++pos_ == page_->end()) {
				page_ = page_->next;
				pos_ = page_->begin();
			}

			return message;
		}

		T pop() {
			T message;
			pop(message);
			return message;
		}
	private:
		std::shared_ptr<Queue> queue_;
		std::shared_ptr<Page> page_;
		size_t offset_;
		T* pos_;
	};

public:
	typedef Listener listener;

	typedef T value_type;

	BroadcastQueue()
	: queue_(new Queue()) {
		//
	}

	listener listen() {
		return Listener(queue_);
	}

	void push(T const &value) {
		queue_->push(value);
	}

private:
	std::shared_ptr<Queue> queue_;
};

} // namespace bitextor

