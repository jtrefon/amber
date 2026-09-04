
#include "agent/semantic_helpers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace agent {

// Tokenize into lowercase alphanumeric words.
std::vector<std::string> tokenize(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            cur += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Deterministic hash of a term into a fixed-dimensional space (hashing trick).
// Keeps memory bounded and avoids a vocab table. This is the embedding step;
// swap this function for a real model to upgrade to dense vector semantics.
void embed(const std::vector<std::string>& terms, std::vector<double>& vec,
           const std::unordered_map<std::string, double>* idf) {
    vec.assign(kEmbedDim, 0.0);
    for (const auto& t : terms) {
        size_t h = std::hash<std::string>{}(t);
        size_t slot = h % kEmbedDim;
        double w = 1.0;
        if (idf) {
            auto it = idf->find(t);
            w = (it != idf->end()) ? it->second : 1.0;
        }
        // sign from a second hash gives a balanced, collision-tolerant vector
        double sign = (h & 1) ? 1.0 : -1.0;
        vec[slot] += sign * w;
    }
}

double cosine(const std::vector<double>& a, const std::vector<double>& b) {
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    if (na == 0.0 || nb == 0.0) return 0.0;
    return dot / (std::sqrt(na) * std::sqrt(nb));
}

bool matches_glob(const std::string& name, const std::string& glob) {
    if (glob.empty()) return true;
    // simple '*' wildcard match
    size_t gi = 0, ni = 0;
    while (gi < glob.size() && ni < name.size()) {
        if (glob[gi] == '*') {
            if (gi + 1 == glob.size()) return true;
            // try to match remainder
            for (size_t k = ni; k <= name.size(); ++k)
                if (matches_glob(name.substr(k), glob.substr(gi + 1))) return true;
            return false;
        }
        if (glob[gi] != name[ni]) return false;
        ++gi; ++ni;
    }
    return gi == glob.size() && ni == name.size();
}

std::string shell_quote(const std::string& s) {
    std::string q = "'";
    for (char c : s) q += (c == '\'') ? "'\\''" : std::string(1, c);
    return q + "'";
}

void walk(const std::string& dir, const std::string& glob,
          const std::vector<std::string>& exclude_dirs,
          std::vector<std::string>& files) {
    // Portable recursive walk via std::filesystem. The old implementation
    // shelled out to GNU `find -readable`, which is a GNU extension absent on
    // macOS/BSD find — so the semantic index was always empty there. Avoid the
    // shell entirely.
    std::error_code ec;
    fs::path root(dir);
    if (!fs::is_directory(root, ec)) return;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
         it != end && !ec; it.increment(ec)) {
        const fs::path& p = it->path();
        if (!fs::is_regular_file(p, ec)) continue;
        // Honor exclude dirs by their top-level name anywhere in the path.
        bool excluded = false;
        for (const auto& d : exclude_dirs) {
            for (const auto& seg : p) {
                if (seg.string() == d) { excluded = true; break; }
            }
            if (excluded) break;
        }
        if (excluded) continue;
        if (!matches_glob(p.filename().string(), glob)) continue;
        files.push_back(p.string());
    }
}

} // namespace agent
