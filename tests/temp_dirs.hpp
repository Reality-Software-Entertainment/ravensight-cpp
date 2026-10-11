// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Temporary directories for tests, removed after each test case.

#pragma once

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "ravensight/client.hpp"
#include "test_runner.hpp"

namespace rstest {

inline std::vector<std::string>& temp_dir_registry() {
    static std::vector<std::string> dirs;
    return dirs;
}

inline void remove_temp_dirs() {
    for (const std::string& dir : temp_dir_registry()) {
        std::error_code error;
        std::filesystem::remove_all(std::filesystem::path(dir), error);
    }
    temp_dir_registry().clear();
}

/// A fresh, not yet created directory under the system temp directory. It
/// is deleted (if it was created) when the current test case ends.
inline std::string make_temp_dir(const std::string& prefix = "ravensight-test-") {
    static const bool hooked = [] {
        after_each_hooks().push_back(&remove_temp_dirs);
        return true;
    }();
    (void)hooked;
    const std::string dir =
        (std::filesystem::temp_directory_path() / (prefix + ravensight::generate_device_id()))
            .string();
    temp_dir_registry().push_back(dir);
    return dir;
}

} // namespace rstest
