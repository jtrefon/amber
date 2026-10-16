trigger: when code shares state across threads, or an async task outlives the call that started it
tags: #cpp #concurrency #async
verified: 2026-10
reference: none

Concurrency bugs are usually ownership bugs wearing a disguise. Before reaching
for a mutex, answer three questions in writing:

- **Who owns the state?** One thread mutating and others reading a snapshot is a
  different design from many writers, and needs a different answer.
- **Who may call this, and from where?** A function that is safe on one thread
  and unsafe on another should say so where it is declared, not in a comment
  three callers later.
- **What cancels it?** A task that outlives the call that started it needs a
  cancellation path and a join at teardown; a detached thread that touches
  caller state is a use-after-free waiting for the right timing.

Then pick the mechanism for the shape you named: one writer and immutable
snapshots need almost no locking; shared mutable state needs a lock whose scope
is one small operation, never a callback. A lock held across I/O or a callback
is where deadlocks come from.
