#include "bfs.h"
#include <cstdio>
int main() {
    int fails = 0;
    {
        std::vector<std::pair<int, int>> edges = {{0, 1}, {1, 2}, {2, 3}};
        auto d = bfs_distances(4, edges, 0);
        std::printf("line: %d %d %d %d\n", d[0], d[1], d[2], d[3]);
        if (d[0] != 0 || d[1] != 1 || d[2] != 2 || d[3] != 3) ++fails;
    }
    {
        std::vector<std::pair<int, int>> edges = {{0, 1}, {0, 2}, {1, 3}};
        auto d = bfs_distances(5, edges, 0);
        std::printf("star: %d %d %d %d %d\n", d[0], d[1], d[2], d[3], d[4]);
        if (d[0] != 0 || d[1] != 1 || d[2] != 1 || d[3] != 2 || d[4] != -1) ++fails;
    }
    {
        auto d = bfs_distances(3, {}, 1);
        std::printf("isolated: %d %d %d\n", d[0], d[1], d[2]);
        if (d[1] != 0 || d[0] != -1 || d[2] != -1) ++fails;
    }
    return fails == 0 ? 0 : 1;
}
