#include "bench/data_root.h"

#include <filesystem>

namespace bench {

namespace fs = std::filesystem;

std::string resolve_bench_root(const std::string& env_override,
                               const std::string& compiled_dir,
                               const std::string& exe_path,
                               const std::string& cwd,
                               const std::function<bool(const std::string&)>& is_dir) {
    if (!env_override.empty())
        return env_override;
    if (!compiled_dir.empty() && is_dir(compiled_dir))
        return compiled_dir;
    if (!exe_path.empty()) {
        const fs::path beside = fs::path(exe_path).parent_path().parent_path() / "share" /
                                "amber-bench";
        if (is_dir(beside.string()))
            return beside.string();
    }
    return (fs::path(cwd) / "bench").string();
}

} // namespace bench
