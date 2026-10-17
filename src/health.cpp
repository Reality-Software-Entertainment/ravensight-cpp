// Copyright 2026 Reality Software Entertainment. MIT licensed.

#include "health.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#elif defined(__linux__)
#include <unistd.h>

#include <fstream>
#include <sstream>
#include <string>
#endif

namespace ravensight {

void HealthSampler::add_frame(double dt_seconds) {
    if (!(dt_seconds > 0.0) || !std::isfinite(dt_seconds)) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    elapsed_ += dt_seconds;
    if (elapsed_ < sample_interval_seconds) {
        return;
    }
    elapsed_ = 0.0;
    const double fps = 1.0 / dt_seconds;
    ++seen_;
    if (samples_.size() < max_samples) {
        samples_.push_back(fps);
        return;
    }
    // Reservoir sampling (xorshift64): every second of the session keeps an
    // equal chance of being in the final set.
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 7;
    rng_ ^= rng_ << 17;
    const std::uint64_t slot = rng_ % seen_;
    if (slot < max_samples) {
        samples_[static_cast<std::size_t>(slot)] = fps;
    }
}

std::size_t HealthSampler::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return samples_.size();
}

bool HealthSampler::summarize(double& fps_median, double& fps_p10) const {
    std::vector<double> sorted;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sorted = samples_;
    }
    if (sorted.empty()) {
        return false;
    }
    std::sort(sorted.begin(), sorted.end());
    const std::size_t n = sorted.size();
    fps_median = (n % 2 == 1) ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2.0;
    const std::size_t rank = static_cast<std::size_t>(std::ceil(0.1 * static_cast<double>(n)));
    fps_p10 = sorted[(rank == 0 ? 1 : rank) - 1];
    return true;
}

#if defined(__APPLE__)

double process_memory_mb() {
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                  &count) != KERN_SUCCESS) {
        return -1.0;
    }
    return static_cast<double>(info.resident_size) / (1024.0 * 1024.0);
}

double process_start_unix_seconds() {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(getpid())};
    struct kinfo_proc proc;
    std::size_t size = sizeof(proc);
    if (sysctl(mib, 4, &proc, &size, nullptr, 0) != 0 || size < sizeof(proc)) {
        return -1.0;
    }
    const struct timeval start = proc.kp_proc.p_un.__p_starttime;
    return static_cast<double>(start.tv_sec) + static_cast<double>(start.tv_usec) / 1e6;
}

#elif defined(__linux__)

double process_memory_mb() {
    std::ifstream statm("/proc/self/statm");
    long long total_pages = 0;
    long long resident_pages = 0;
    if (!(statm >> total_pages >> resident_pages)) {
        return -1.0;
    }
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        return -1.0;
    }
    return static_cast<double>(resident_pages) * static_cast<double>(page) / (1024.0 * 1024.0);
}

double process_start_unix_seconds() {
    const long ticks = sysconf(_SC_CLK_TCK);
    if (ticks <= 0) {
        return -1.0;
    }
    std::ifstream stat_file("/proc/self/stat");
    std::string line;
    if (!std::getline(stat_file, line)) {
        return -1.0;
    }
    // The command name is in parentheses and may hold spaces: count fields
    // after the last ')'. Field 22 (starttime) is the 20th one after it.
    const std::size_t close = line.rfind(')');
    if (close == std::string::npos) {
        return -1.0;
    }
    std::istringstream rest(line.substr(close + 1));
    std::string field;
    unsigned long long start_ticks = 0;
    for (int index = 3; index <= 22; ++index) {
        if (!(rest >> field)) {
            return -1.0;
        }
        if (index == 22) {
            start_ticks = std::strtoull(field.c_str(), nullptr, 10);
        }
    }
    std::ifstream stat_all("/proc/stat");
    double boot_time = -1.0;
    while (std::getline(stat_all, line)) {
        if (line.compare(0, 6, "btime ") == 0) {
            boot_time = std::strtod(line.c_str() + 6, nullptr);
            break;
        }
    }
    if (boot_time < 0.0) {
        return -1.0;
    }
    return boot_time + static_cast<double>(start_ticks) / static_cast<double>(ticks);
}

#else

// Windows, Android, consoles: not exposed here. The health event then
// carries frame rates only.
double process_memory_mb() { return -1.0; }
double process_start_unix_seconds() { return -1.0; }

#endif

} // namespace ravensight
