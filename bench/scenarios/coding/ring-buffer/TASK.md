# Task: ring buffer

Implement `RingBuffer` in `ring_buffer.cpp` (class declaration in
`ring_buffer.h`): a fixed-capacity FIFO with:

- `explicit RingBuffer(size_t capacity)`
- `bool push(int value)` — false when full (no overwrite)
- `bool pop(int& value)` — false when empty
- `size_t size() const`
- `bool empty() const`

When full, `push` must fail (not overwrite). Do not change the header
signatures. No standard containers other than the internal array.
