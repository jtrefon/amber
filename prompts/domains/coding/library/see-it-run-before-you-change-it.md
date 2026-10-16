name: see it run before you change it
trigger: when a failure only shows in the running program, or a change needs a feedback loop first
tags: #debugging #observability #testing
weight: 65
reference: none

Reading code suggests a hypothesis; running it tests one. Climb the ladder from
cheapest to most informative, and stop at the first rung that answers the
question:

1. A test that fails for the reason you suspect — the cheapest possible loop.
2. The program, with logging turned up: run it, read what it says, keep it
   running in the background so you can look again.
3. A debugger: break where the state should be right, and check whether it is.
4. A trace or a profiler, when the question is "how does it get here" or "why is
   this slow" rather than "what is wrong here".

Logs from a real run contain real values, and real values contain secrets: read
them, quote the shape, and keep the values out of anything that outlives the
session.
