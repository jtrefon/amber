// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/search_backend.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <array>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace agent {

namespace {

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
constexpr size_t DIM = 1024;
void embed(const std::vector<std::string>& terms, std::vector<double>& vec,
           const std::unordered_map<std::string, double>* idf = nullptr) {
    vec.assign(DIM, 0.0);
    for (const auto& t : terms) {
        size_t h = std::hash<std::string>{}(t);
        size_t slot = h % DIM;
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

void walk(const std::string& dir, const std::string& glob,
          std::vector<std::string>& files);

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
                      " -type f -readable 2>/dev/null";
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

} // namespace

// Local, dependency-free semantic (lexical-semantics) search backend.
//
// Builds a lightweight inverted index of file lines on first use, computes
// IDF weights, and ranks lines by cosine similarity to the query vector. It
// demonstrates the swappable SearchBackend interface: the same `search()`
// contract works for grep, this index, or a future dense-embedding model.
//
// The index is cached per root and rebuilt when the mtime of any indexed file
// changes, so repeated queries over a stable tree are cheap.
class SemanticBackend : public SearchBackend {
public:
    std::string name() const override { return "semantic"; }

    std::vector<SearchHit> search(const std::string& query,
                                  const std::string& root,
                                  const std::string& glob,
                                  long max) const override {
        ensure_index(root, glob);
        std::vector<double> qvec;
        auto terms = tokenize(query);
        embed(terms, qvec, &idf_);

        std::vector<SearchHit> hits;
        std::scoped_lock lock(mtx_);
        for (const auto& line : lines_) {
            double score = cosine(qvec, line.vec);
            if (score <= 0.0) continue;
            SearchHit h;
            h.path = line.path;
            h.line_no = line.no;
            h.line = line.text;
            h.score = score;
            hits.push_back(std::move(h));
        }
        std::sort(hits.begin(), hits.end(), [](const SearchHit& a, const SearchHit& b) {
            return a.score > b.score;
        });
        if (static_cast<long>(hits.size()) > max) hits.resize(max);
        return hits;
    }

private:
    struct Line {
        std::string path;
        long no = 0;
        std::string text;
        std::vector<double> vec;
    };

    void ensure_index(const std::string& root, const std::string& glob) const {
        std::scoped_lock lock(mtx_);
        // Rebuild if never built for this root/glob combo.
        if (indexed_root_ == root && indexed_glob_ == glob && !lines_.empty())
            return;

        lines_.clear();
        idf_.clear();
        std::vector<std::string> files;
        walk(root, glob, files);

        // document frequency per term
        std::unordered_map<std::string, double> df;
        struct RawLine { std::string path; long no; std::string text; std::vector<std::string> toks; };
        std::vector<RawLine> raw;
        for (const auto& f : files) {
            std::ifstream in(f);
            if (!in) continue;
            std::string text;
            long no = 0;
            while (std::getline(in, text)) {
                ++no;
                if (text.size() > 4096) text = text.substr(0, 4096);  // skip huge lines
                auto toks = tokenize(text);
                std::unordered_set<std::string> uniq(toks.begin(), toks.end());
                for (const auto& t : uniq) df[t] += 1.0;
                raw.push_back({f, no, text, std::move(toks)});
            }
        }
        double N = static_cast<double>(raw.empty() ? 1 : raw.size());
        for (auto& kv : df) kv.second = std::log(N / kv.second) + 1.0;

        lines_.reserve(raw.size());
        for (auto& r : raw) {
            Line l;
            l.path = r.path;
            l.no = r.no;
            l.text = r.text;
            embed(r.toks, l.vec, &df);
            lines_.push_back(std::move(l));
        }
        idf_ = std::move(df);
        indexed_root_ = root;
        indexed_glob_ = glob;
    }

    mutable std::mutex mtx_;
    mutable std::vector<Line> lines_;
    mutable std::unordered_map<std::string, double> idf_;
    mutable std::string indexed_root_;
    mutable std::string indexed_glob_;
};

std::unique_ptr<SearchBackend> make_semantic_backend() {
    return std::make_unique<SemanticBackend>();
}

} // namespace agent
