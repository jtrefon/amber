# Task: extract methods

`process.cpp` contains one function `process(int a, int b, int c)` that does
everything inline. Refactor it into small named helper functions so that
`process` itself reads as a sequence of calls. Name your helpers with the
prefix `extract_` (for example `extract_normalize`, `extract_score`,
`extract_render`). Behavior must not change: the hidden tests compare the
exact output of `process` before and after.

Do not change the signature of `process`. Do not touch `process.h`.
