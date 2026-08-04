
#ifndef BENCH_RESOURCES_H
#define BENCH_RESOURCES_H

// Process resource sampling via getrusage (POSIX, linux + darwin).
// ru_maxrss is KiB on Linux but BYTES on macOS — normalized to KiB here.
// ru_maxrss is monotonic, so "peak during run" is the max seen at stop().

namespace bench {

class ResourceMeter {
public:
    void start() noexcept;
    void stop() noexcept;

    long baseline_rss_kb() const noexcept { return baseline_kb_; }
    long peak_rss_kb() const noexcept { return peak_kb_; }
    long cpu_ms() const noexcept { return cpu_ms_; }
    bool started() const noexcept { return started_; }

private:
    bool started_ = false;
    long baseline_kb_ = 0;
    long peak_kb_ = 0;
    long cpu_ms_ = 0;
};

} // namespace bench

#endif // BENCH_RESOURCES_H
