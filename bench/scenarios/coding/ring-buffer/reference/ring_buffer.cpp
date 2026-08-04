#include "ring_buffer.h"

RingBuffer::RingBuffer(size_t capacity)
    : cap_(capacity), buf_(new int[capacity]) {}

bool RingBuffer::push(int value) {
    if (count_ == cap_) return false;
    buf_[(head_ + count_) % cap_] = value;
    ++count_;
    return true;
}

bool RingBuffer::pop(int& value) {
    if (count_ == 0) return false;
    value = buf_[head_];
    head_ = (head_ + 1) % cap_;
    --count_;
    return true;
}

size_t RingBuffer::size() const { return count_; }
bool RingBuffer::empty() const { return count_ == 0; }
