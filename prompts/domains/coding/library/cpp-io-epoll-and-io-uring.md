trigger: when a program handles many sockets, or blocking IO in threads has stopped scaling
tags: #cpp #io
weight: 50
verified: 2026-10
sources: model knowledge 2026-10; claims executed by their run block
reference: https://man7.org/linux/man-pages/man7/epoll.7.html

Two models, one question: who waits, and how many syscalls does an event cost?
`epoll` answers with readiness; `io_uring` answers with completion, and changes
what "async" costs.

- **epoll is readiness-based**: one `epoll_wait` returns the descriptors that are
  *ready*, and you still do the read/write yourself. Level-triggered re-reports
  until handled; edge-triggered reports once, so you must drain until `EAGAIN` —
  the classic source of a stalled server.
- **`EPOLLOUT` is almost always a mistake to watch continuously**: write when the
  socket is writable and it usually is; register for `EPOLLOUT` only when a write
  returned `EAGAIN`, and deregister when it drains.
- **Non-blocking is not optional.** A blocking read in a readiness loop stops the
  whole loop; `accept` must be called in a loop until `EAGAIN` too.
- **io_uring is completion-based**: you submit reads and writes with a buffer
  and get completions back — real async, fewer syscalls, and the same code shape
  for files and sockets (which is the part epoll cannot do: regular files are
  always "ready").
- **Its traps are lifetime ones**: a buffer used by an in-flight operation must
  not move or be freed. Registered buffers and fixed files exist for that reason.
- **A thread pool plus blocking IO is a legitimate answer** below a few hundred
  connections; complexity is a cost, and `epoll` is only the cheaper choice past
  the point where threads stop fitting.

## Run it

```run
# manual: needs a listening socket and a client; the shape is epoll_wait -> accept -> read until EAGAIN
echo "see the reference for the edge-triggered drain rule"
```
