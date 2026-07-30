
#ifndef AGENT_SEMANTIC_HELPERS_H
#define AGENT_SEMANTIC_HELPERS_H

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace agent {

// Hashing-trick dimensionality for the lexical embedding.
constexpr size_t kEmbedDim = 1024;

// Tokenize into lowercase alphanumeric words.
std::vector<std::string> tokenize(const std::string& s);

// Deterministic hash of terms into a fixed-dimensional space (hashing trick).
// Keeps memory bounded and avoids a vocab table. Swap for a real model to
// upgrade to dense vector semantics.
void embed(const std::vector<std::string>& terms, std::vector<double>& vec,
           const std::unordered_map<std::string, double>* idf = nullptr);

double cosine(const std::vector<double>& a, const std::vector<double>& b);

bool matches_glob(const std::string& name, const std::string& glob);

// Recursive file discovery (via `find`) honoring a glob on the basename.
void walk(const std::string& dir, const std::string& glob,
          std::vector<std::string>& files);

} // namespace agent

#endif
