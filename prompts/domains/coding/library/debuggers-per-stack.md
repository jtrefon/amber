trigger: when a failure has no reproduction, or the state on the machine disagrees with the code
tags: #debugging #gdb #tooling
weight: 60
verified: 2026-10
reference: https://sourceware.org/gdb/current/onlinedocs/gdb.html
sources: Wang, Systems Programming in Unix/Linux, Ch. 2.8; Weiss, System Programming in Linux (2025), Ch. 8; Cosenza "Linux Core-Dump Triage with systemd-coredump, coredumpctl, and GDB" (2026); web pass 2026-10: delve, dotnet-dump, rr docs

Attach before restarting: the running process still holds the state the crash
had, and a debugger reads it without changing the workload.

- **Native**: `gdb -p PID` or `lldb -p PID`; `gdb --args ./app` for a fresh
  run. A conditional breakpoint on the invariant (`break f.cpp:42 if n != 0`)
  is worth ten single-steps, and a watchpoint (`watch counter`) answers "who
  wrote this?" in one stop — reach for it when a value is wrong, not when a
  line is reached.
- **Python**: `breakpoint()` (3.7+) drops to pdb, `python -m pdb script.py`,
  ipdb for a better prompt, `pdb.post_mortem()` inside the except clause,
  `pytest --pdb`. `faulthandler.enable()` prints the Python stack on a fatal
  signal from a process that was never under a debugger.
- **Node**: `node inspect app.js` or `--inspect-brk` and the DevTools
  protocol; `kill -USR1 PID` opens the inspector on an already-running
  process; a core plus llnode for post-mortem reading.
- **Go**: `dlv debug` or `dlv attach PID`; breakpoints by function or line and
  goroutine-aware inspection; `dlv core ./bin ./core` reads a dump. SIGQUIT
  prints every goroutine's stack with no debugger at all.
- **Java**: `jdb` for source-level stepping; attach with the JDWP agent
  (`-agentlib:jdwp=transport=dt_socket,server=y,...`); `jcmd` or `jstack` for
  a live thread dump; `jhsdb jstack --core` or a heap dump once the process is
  gone.
- **.NET**: `dotnet-dump collect -p PID`, then `dotnet-dump analyze` commands
  (`clrstack`, `pe`, `dumpheap`); `dotnet-gcdump` for the managed heap. SOS
  gives the same views inside WinDbg or lldb.

Core dumps are the post-mortem currency: `ulimit -c unlimited`, plus whatever
`kernel.core_pattern` routes to; on systemd hosts `coredumpctl list` then
`coredumpctl debug` reaches the dump. Keep the exact binary and let debuginfod
fetch symbols by build-id — a backtrace against mismatched symbols is a story,
not evidence.

When the failure is a race, reach for time travel: `rr record ./app`, then
`rr replay`, where reverse-continue and reverse-step walk back to the last
write. It is Linux x86-64 and it is slow, but a recorded trace replays
deterministically, which a core cannot.

No repro is normal, not exceptional: capture signal, build id, command line
and environment together with the dump, and treat a backtrace without matching
symbols as an unverified signature. If the core is truncated or missing, say
so rather than narrating a complete one.

## Run it

```run
# manual: debuggers and crash artifacts are not available in CI; post-mortem
# needs a real core. The shapes are:
#   gdb -p PID                          # attach to a live process
#   coredumpctl debug PID               # systemd route to a recorded core
#   rr record ./app && rr replay        # record once, replay forever
gdb --version
```
