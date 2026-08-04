#include "calc.h"
int total(const int* xs) {
    int sum = 0;
    for (int i = 0; xs[i] != -1; ++i)
        sum += xs[i + 1];
    return sum;
}
