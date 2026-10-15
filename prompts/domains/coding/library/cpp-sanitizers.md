name: sanitizers before the debugger
trigger: when memory, lifetime or indexing behaviour is in question — a sanitizer names the fault, a debugger waits for it
tags: #cpp #debugging #testing #sanitizers
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
