// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Fake transports record what the server would read: a gzip body decoded,
// with the Content-Encoding header kept so a test can still see it was sent.

#pragma once

#include "delivery.hpp"
#include "ravensight/transport.hpp"

namespace rstest {

inline ravensight::Request decoded(const ravensight::Request& raw) {
    ravensight::Request request = raw;
    for (const auto& header : raw.headers) {
        if (header.first == "Content-Encoding" && header.second == "gzip") {
            request.body = ravensight::detail::gzip_decompress(raw.body);
        }
    }
    return request;
}

} // namespace rstest
