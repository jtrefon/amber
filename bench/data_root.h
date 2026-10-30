#ifndef BENCH_DATA_ROOT_H
#define BENCH_DATA_ROOT_H

#include <functional>
#include <string>

namespace bench {

// Where the scenario corpus and profiles.json live, in order of preference:
//   1. the directory ./configure stamped into the binary
//   2. <exe>/../share/amber-bench, for a relocatable install
//   3. <cwd>/bench, for a source checkout
//
// There is deliberately no environment override: this path reaches the compiler
// invocation the harness builds, and the three cases above are all of them.
//
// The predicate answers "is this a directory". Injecting it keeps the ordering
// testable without a filesystem.
std::string resolve_bench_root(const std::string& compiled_dir, const std::string& exe_path,
                               const std::string& cwd,
                               const std::function<bool(const std::string&)>& is_dir);

} // namespace bench

#endif // BENCH_DATA_ROOT_H
