name: iterator invalidation
trigger: when a container is mutated while something still holds an iterator or a reference into it
tags: #cpp #containers #memory
weight: 60
reference: none

The mutation is usually the safe-looking one: inserting into a map is fine, the
iterator to a different element is not; a vector's `push_back` can reallocate
everything. Name the container's invalidation rule before touching it:

- `std::vector` / `std::string`: any reallocation invalidates every pointer,
  reference and iterator. `reserve` first when the size is known.
- `std::deque`: insertions in the middle invalidate all; at the ends, iterators.
- `std::map` / `std::set`: insertions never invalidate; erasures invalidate only
  what they erased.
- `std::unordered_*`: rehashing invalidates iterators but not references or
  pointers to elements.

The way out is usually to collect what to remove, or index by key, rather than
to hold an iterator across a mutation.
