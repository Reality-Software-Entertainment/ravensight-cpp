// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// The built in transport over libcurl. TLS, proxies and certificates are
// libcurl's job; this file only shapes requests and captures responses.

#pragma once

#include "ravensight/transport.hpp"

namespace ravensight {

/// Blocking transport over a single reused libcurl easy handle. Reusing the
/// handle keeps the HTTPS connection alive between flushes. Not reentrant:
/// the Client only ever calls send from its one worker thread.
class CurlTransport : public Transport {
public:
    explicit CurlTransport(double timeout_seconds = 15.0);
    ~CurlTransport() override;

    CurlTransport(const CurlTransport&) = delete;
    CurlTransport& operator=(const CurlTransport&) = delete;

    Response send(const Request& request) override;

private:
    void* handle_ = nullptr; // CURL*
    double timeout_seconds_;
};

} // namespace ravensight
