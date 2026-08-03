
#include "bench/resources.h"

#include <sys/resource.h>

#ifdef __APPLE__
#include <mach/mach.h>
#endif

namespace bench {

namespace {

long maxrss_kb(const struct rusage& ru) noexcept {
    // Linux: ru_maxrss is KiB. macOS: ru_maxrss is BYTES.
#ifdef __APPLE__
    return static_cast<long>(ru.ru_maxrss / 1024);
#else
    return static_cast<long>(ru.ru_maxrss);
#endif
}

long rusage_cpu_ms(const struct rusage& ru) noexcept {
    auto secs = [](const struct timeval& t) {
        return (static_cast<long>(t.tv_sec) * 1000) + (t.tv_usec / 1000);
    };
    return (secs(ru.ru_utime) + secs(ru.ru_stime));
}

} // namespace

void ResourceMeter::start() noexcept {
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return;
    baseline_kb_ = maxrss_kb(ru);
    cpu_ms_ = rusage_cpu_ms(ru);
    started_ = true;
}

void ResourceMeter::stop() noexcept {
    if (!started_) return;
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) return;
    // ru_maxrss is monotonic — the value at stop is the process peak.
    peak_kb_ = maxrss_kb(ru);
    cpu_ms_ = rusage_cpu_ms(ru) - cpu_ms_;
    started_ = false;
}

} // namespace bench
