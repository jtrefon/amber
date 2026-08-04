#pragma once
#include <cstddef>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity);
    bool push(int value);
    bool pop(int& value);
    size_t size() const;
    bool empty() const;

private:
    size_t cap_;
    int* buf_;
    size_t head_ = 0;
    size_t count_ = 0;
};
