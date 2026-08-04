#include "ring_buffer.h"

RingBuffer::RingBuffer(size_t capacity) : cap_(capacity), buf_(new int[capacity]) {}

bool RingBuffer::push(int) { return false; }
bool RingBuffer::pop(int&) { return false; }
size_t RingBuffer::size() const { return 0; }
bool RingBuffer::empty() const { return true; }
