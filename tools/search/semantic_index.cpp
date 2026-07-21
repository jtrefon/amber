// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/semantic_helpers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <fstream>

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
          std::vector<std::string>& files) {
    // portable recursive directory walk via nftw is not used to avoid extra
    // deps; use a simple popen to `find` which is universally available on the
    // target Linux servers.
    std::string cmd = "find " + shell_quote(dir) +
                      " -type f -readable"
                      " -not -path '*/.amber/*' -not -path '*/.git/*'"
                      " -not -path '*/third_party/*' 2>/dev/null";
    std::array<char, 512> buf{};
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return;
    std::string line;
    while (fgets(buf.data(), static_cast<int>(buf.size()), p)) {
        line = buf.data();
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        if (line.empty()) continue;
        std::string base = line.substr(line.find_last_of('/') + 1);
        if (matches_glob(base, glob)) files.push_back(line);
    }
    pclose(p);
}

} // namespace agent
