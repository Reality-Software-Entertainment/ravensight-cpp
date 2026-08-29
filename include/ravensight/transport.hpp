// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// The transport seam. Deliberately tiny so the batching and retry logic can
// be exercised with a fake transport in a plain unit test, with no network
// or libcurl involved.

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "ravensight/json.hpp"

namespace ravensight {

/// One outbound HTTP request.
struct Request {
    std::string method;
    std::string url;
    /// Serialized JSON body, empty for a GET.
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;

    Request& with_header(std::string name, std::string value) {
        headers.emplace_back(std::move(name), std::move(value));
        return *this;
    }
};

/// Retry-After in seconds, or 0 when the value is absent, negative or not the
/// delta seconds form. The HTTP-date form is legal but the API sends delta
/// seconds, so anything else falls back to the caller's exponential backoff.
double parse_retry_after_seconds(const std::string& header_value);

/// One inbound HTTP response. A transport level failure (no connection, DNS,
/// timeout) is reported as status 0 so the caller treats it like any other
/// retryable outcome instead of throwing into game code.
struct Response {
    long status = 0;
    std::string body;
    /// Raw Retry-After header value, empty when absent.
    std::string retry_after;

    static Response network_failure(std::string reason) {
        Response response;
        response.status = 0;
        response.body = std::move(reason);
        return response;
    }

    /// Parsed body, a null Value when the body was absent or malformed.
    json::Value parse_json() const { return json::Value::parse(body); }

    /// The server's "error" field if it sent one, else the fallback.
    std::string error_code(const std::string& fallback) const {
        return parse_json().get_string("error", fallback);
    }

    double retry_after_seconds() const { return parse_retry_after_seconds(retry_after); }
};

/// Sends one request, blocking, and never throws. Implementations must
/// translate every failure into a Response with status 0. The Client calls
/// send from its single worker thread only, so implementations do not need
/// to be reentrant.
class Transport {
public:
    virtual ~Transport() = default;
    virtual Response send(const Request& request) = 0;
};

} // namespace ravensight
