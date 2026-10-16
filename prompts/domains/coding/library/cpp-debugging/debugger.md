trigger: when a failure resists two hypotheses, or state is not what the code says it should be
tags: #cpp #debugging #gdb
verified: 2026-10
reference: none

After two wrong hypotheses, the next step is a debugger rather than a third.
Build with `-g` (and `-O0` if the optimised frame lies), then:

- `gdb --args <binary> <args>` or `lldb -- <binary> <args>`; `bt` first, so the
  shape of the failure is known before any stepping.
- A conditional breakpoint on the invariant (`break file.cpp:120 if x != 0`) is
  worth ten breakpoints on a loop.
- `watch <expression>` stops where the value changes, which is the question
  "who wrote this?" answerable in one command.
- A core dump is a recording of the failure: `gdb <binary> core` when the crash
  is intermittent or the process is already gone.
- `p *ptr` on a freed pointer, and the sanitizer's own report, answer different
  halves of the same question — use both when the fault is a lifetime one.

Keep the command that reproduced it in your notes: a debuggable reproduction is
most of the fix.
