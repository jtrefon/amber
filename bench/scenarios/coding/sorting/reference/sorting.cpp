#include "sorting.h"

void bubble_sort(int* a, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        bool swapped = false;
        for (size_t j = 0; j + 1 < n - i; ++j) {
            if (a[j] > a[j + 1]) {
                int tmp = a[j];
                a[j] = a[j + 1];
                a[j + 1] = tmp;
                swapped = true;
            }
        }
        if (!swapped) break;
    }
}

static void merge(int* a, size_t lo, size_t mid, size_t hi) {
    size_t i = lo, j = mid;
    int* tmp = new int[hi - lo];
    size_t k = 0;
    while (i < mid && j < hi) tmp[k++] = a[i] <= a[j] ? a[i++] : a[j++];
    while (i < mid) tmp[k++] = a[i++];
    while (j < hi) tmp[k++] = a[j++];
    for (size_t t = 0; t < k; ++t) a[lo + t] = tmp[t];
    delete[] tmp;
}

void merge_sort(int* a, size_t n) {
    if (n <= 1) return;
    size_t mid = n / 2;
    merge_sort(a, mid);
    merge_sort(a + mid, n - mid);
    merge(a, 0, mid, n);
}
