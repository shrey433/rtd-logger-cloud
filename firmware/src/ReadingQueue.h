#pragma once
#include <stddef.h>
#include <stdint.h>

constexpr int kChannels = 8;

struct Reading {
  uint32_t id;               // increases with every sample; ties RAM rows to their flash copy
  uint32_t ts;               // Unix epoch seconds, UTC
  float temp_c[kChannels];   // NAN when the MAX31865 reports a fault
};

// Fixed-size ring buffer. When full, the oldest row is overwritten so the newest data survives.
class ReadingQueue {
 public:
  void begin(Reading* storage, size_t capacity) {
    buf_ = storage;
    cap_ = capacity;
    head_ = 0;
    count_ = 0;
    dropped_ = 0;
  }

  void push(const Reading& r) {
    if (cap_ == 0) return;
    if (count_ == cap_) {
      head_ = (head_ + 1) % cap_;
      count_--;
      dropped_++;
    }
    buf_[(head_ + count_) % cap_] = r;
    count_++;
  }

  bool peekOldest(Reading& out) const {
    if (count_ == 0) return false;
    out = buf_[head_];
    return true;
  }

  void popOldest() {
    if (count_ == 0) return;
    head_ = (head_ + 1) % cap_;
    count_--;
  }

  size_t size() const { return count_; }
  size_t capacity() const { return cap_; }
  uint32_t dropped() const { return dropped_; }

 private:
  Reading* buf_ = nullptr;
  size_t cap_ = 0;
  size_t head_ = 0;
  size_t count_ = 0;
  uint32_t dropped_ = 0;
};
