#include "bfs.h"

#include <queue>

std::vector<int> bfs_distances(int n, const std::vector<std::pair<int, int>>& edges,
                               int source) {
    std::vector<std::vector<int>> adj(n);
    for (const auto& e : edges) {
        adj[e.first].push_back(e.second);
        adj[e.second].push_back(e.first);
    }
    std::vector<int> dist(n, -1);
    std::queue<int> q;
    dist[source] = 0;
    q.push(source);
    while (!q.empty()) {
        const int u = q.front();
        q.pop();
        for (const int v : adj[u]) {
            if (dist[v] != -1) continue;
            dist[v] = dist[u] + 1;
            q.push(v);
        }
    }
    return dist;
}
