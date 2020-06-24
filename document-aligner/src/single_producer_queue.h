#pragma once
#include <memory>
#include "util/pcqueue.hh"

namespace bitextor {

/**
 * Single producer queue. Straight up copy of KPU's preprocess library with
 * the addition of an Empty() method that does non-blocking checking whether
 * we might be at the end of the queue.
 */
template <class T> class SingleProducerQueue {
  public:
  	typedef util::UnboundedPage<T> Page;

    SingleProducerQueue() : valid_(0) {
      SetFilling(new Page());
      SetReading(filling_);
    }

    void Produce(const T &val) {
      if (filling_current_ == filling_end_) {
        Page *next = new Page();
        filling_->next = next;
        SetFilling(next);
      }
      *(filling_current_++) = val;
      valid_.post();
    }

    T& Consume(T &out) {
      util::WaitSemaphore(valid_);
      if (reading_current_ == reading_end_) {
        SetReading(reading_->next);
      }
      out = *(reading_current_++);
      return out;
    }

    // Warning: very much a no-guarantees race-condition-rich implementation!
    // But sufficient for our specific purpose.
    bool Empty() const {
      return reading_current_ == filling_current_;
    }

  private:
    void SetFilling(Page *to) {
      filling_ = to;
      filling_current_ = to->entries;
      filling_end_ = filling_current_ + sizeof(to->entries) / sizeof(T);
    }

    void SetReading(Page *to) {
      reading_.reset(to);
      reading_current_ = to->entries;
      reading_end_ = reading_current_ + sizeof(to->entries) / sizeof(T);
    }

    util::Semaphore valid_;

    Page *filling_;

    std::unique_ptr<Page> reading_;

    T *filling_current_;
    T *filling_end_;
    T *reading_current_;
    T *reading_end_;

    SingleProducerQueue(const SingleProducerQueue &) = delete;
    SingleProducerQueue &operator=(const SingleProducerQueue &) = delete;
};

} // namespace