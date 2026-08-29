// Copyright 2026 Reality Software Entertainment. MIT licensed.

#include "ravensight/transport.hpp"

#include <cmath>
#include <cstdlib>

namespace ravensight {

double parse_retry_after_seconds(const std::string& header_value) {
    if (header_value.empty()) {
        return 0.0;
    }
    // Trim surrounding whitespace.
    std::size_t begin = header_value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return 0.0;
    }
    std::size_t end = header_value.find_last_not_of(" \t\r\n");
    const std::string trimmed = header_value.substr(begin, end - begin + 1);

    char* parse_end = nullptr;
    const double seconds = std::strtod(trimmed.c_str(), &parse_end);
    if (parse_end == nullptr || *parse_end != '\0') {
        return 0.0;
    }
    if (seconds < 0.0 || std::isnan(seconds) || std::isinf(seconds)) {
        return 0.0;
    }
    return seconds;
}

} // namespace ravensight
