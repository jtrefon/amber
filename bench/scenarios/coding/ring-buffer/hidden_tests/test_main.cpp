#include "ring_buffer.h"
#include <cstdio>
int main() {
    RingBuffer rb(3);
    int fails = 0;
    std::printf("empty=%d size=%zu\n", rb.empty() ? 1 : 0, rb.size());
    if (!rb.empty() || rb.size() != 0) ++fails;
    if (rb.push(1) && rb.push(2) && rb.push(3) && !rb.push(4)) {
        std::printf("full-reject-ok\n");
    } else { std::printf("full-reject-bad\n"); ++fails; }
    int v = 0;
    bool p1 = rb.pop(v), p2 = rb.pop(v), p3 = rb.pop(v);
    std::printf("pops=%d%d%d last=%d size=%zu\n", p1, p2, p3, v, rb.size());
    if (!(p1 && p2 && p3 && v == 3 && rb.empty())) ++fails;
    return fails == 0 ? 0 : 1;
}
