# Task: adapter pattern

`legacy.h` exposes `LegacyLogger` with `void log(const char* line)`. The new
contract (in `adapter.h`) is `LoggerAdapter` with `void info(const std::string&)`
and `void error(const std::string&)`. Create the adapter that maps the new
contract onto the legacy interface, delegating info/error to log() with
"[INFO] "/"[ERROR] " prefixes. Behavior must not change: hidden tests compare
exact output before and after. Do not change `legacy.h`, `adapter.h`, or
`legacy.cpp`.
