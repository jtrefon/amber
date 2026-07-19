// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/search_backend.h"
#include "agent/semantic_helpers.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace agent {

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
                if (text.size() > 4096) text.resize(4096);  // skip huge lines
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
