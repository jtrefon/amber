trigger: when memory, lifetime or indexing behaviour is in question — a sanitizer names the fault, a debugger waits for it
tags: #cpp #debugging #testing #sanitizers
verified: 2026-10
reference: none

Undefined behaviour does not need a debugger; it needs a build that reports it.
Compile the test or the binary with the sanitizer for the failure you suspect and
run the reproduction under it:

- `-fsanitize=address` — use-after-free, leaks, buffer overruns, and it prints
  the allocation and the free site rather than the crash site.
- `-fsanitize=undefined` — signed overflow, bad shifts, misaligned access, null
  dereference.
- `-fsanitize=thread` — data races, with both stacks. Address sanitizer cannot
  see these.
- `-fsanitize=memory` (clang) — reads of uninitialised memory.

Add `-g -fno-omit-frame-pointer` so the report is readable, and keep `-O1` when
the bug only shows optimised. Run the smallest reproduction, not the whole suite:
the first report is usually the cause and the rest are fallout.

## Run it

```run
cat > probe.cpp <<'SRC'
int main() {
    int* p = new int[4];
    delete[] p;
    return *p;
}
SRC
g++ -fsanitize=address -g -O1 probe.cpp -o probe
grep -q "heap-use-after-free" <(./probe 2>&1 || true)
```
