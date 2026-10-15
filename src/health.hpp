// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Frame rate sampling behind Client::sample_frame, and the process facts
// (memory, boot time) the health summary carries. Internal: not installed.

#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace ravensight {

/// Keeps a bounded, uniform reservoir of frames per second samples, one
/// taken for each second of game time. Thread safe.
class HealthSampler {
public:
    static constexpr std::size_t max_samples = 600;
    static constexpr double sample_interval_seconds = 1.0;

    /// Adds the length of one rendered frame. A sample (1 / dt_seconds) is
    /// taken each time a full second has passed since the last one. A
    /// non positive or non finite dt is ignored.
    void add_frame(double dt_seconds);

    std::size_t count() const;

    /// Median and 10th percentile (nearest rank, the slow end) of the
    /// samples taken so far. Returns false when none were taken.
    bool summarize(double& fps_median, double& fps_p10) const;

private:
    mutable std::mutex mutex_;
    std::vector<double> samples_;
    std::uint64_t seen_ = 0;
    double elapsed_ = 0.0;
    std::uint64_t rng_ = 0x9E3779B97F4A7C15ULL;
};

/// Resident memory of this process in megabytes, or a negative number
/// where the platform does not expose it.
double process_memory_mb();

/// Unix time (seconds) the process started, or a negative number where the
/// platform does not expose it.
double process_start_unix_seconds();

} // namespace ravensight
