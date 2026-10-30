#ifndef BENCH_DATA_ROOT_H
#define BENCH_DATA_ROOT_H

#include <functional>
#include <string>

namespace bench {

// Where the scenario corpus and profiles.json live, in order of preference:
//   1. the AMBER_BENCH_DATA override, which wins outright (a run may point at a
//      corpus it fetched, and silently falling back would hide a typo)
//   2. the directory ./configure stamped into the binary
//   3. <exe>/../share/amber-bench, for a relocatable install
//   4. <cwd>/bench, for a source checkout
//
// The predicate answers "is this a directory". Injecting it keeps the ordering
// testable without a filesystem.
std::string resolve_bench_root(const std::string& env_override, const std::string& compiled_dir,
                               const std::string& exe_path, const std::string& cwd,
                               const std::function<bool(const std::string&)>& is_dir);

} // namespace bench

#endif // BENCH_DATA_ROOT_H
