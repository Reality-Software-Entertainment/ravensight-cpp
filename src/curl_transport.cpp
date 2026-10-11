// Copyright 2026 Reality Software Entertainment. MIT licensed.

#include "ravensight/curl_transport.hpp"
#include "ravensight/version.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <mutex>

#include <curl/curl.h>

namespace ravensight {

namespace {

std::once_flag g_curl_global_init;

struct Capture {
    std::string body;
    std::string retry_after;
};

size_t write_callback(char* data, size_t size, size_t count, void* userdata) {
    Capture* capture = static_cast<Capture*>(userdata);
    capture->body.append(data, size * count);
    return size * count;
}

size_t header_callback(char* data, size_t size, size_t count, void* userdata) {
    Capture* capture = static_cast<Capture*>(userdata);
    const size_t total = size * count;
    const std::string line(data, total);

    static const char prefix[] = "retry-after:";
    const size_t prefix_len = sizeof(prefix) - 1;
    if (line.size() > prefix_len) {
        bool match = true;
        for (size_t i = 0; i < prefix_len; ++i) {
            if (std::tolower(static_cast<unsigned char>(line[i])) != prefix[i]) {
                match = false;
                break;
            }
        }
        if (match) {
            std::string value = line.substr(prefix_len);
            const size_t begin = value.find_first_not_of(" \t");
            const size_t end = value.find_last_not_of(" \t\r\n");
            if (begin != std::string::npos && end != std::string::npos && end >= begin) {
                capture->retry_after = value.substr(begin, end - begin + 1);
            }
        }
    }
    return total;
}

double wall_clock_seconds() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Aborts the transfer once the request's shutdown deadline passes.
int progress_callback(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const std::atomic<double>* abort_at = static_cast<const std::atomic<double>*>(userdata);
    if (abort_at == nullptr) {
        return 0;
    }
    const double deadline = abort_at->load();
    return (deadline > 0.0 && wall_clock_seconds() >= deadline) ? 1 : 0;
}

} // namespace

CurlTransport::CurlTransport(double timeout_seconds)
    : timeout_seconds_(timeout_seconds > 0.0 ? timeout_seconds : 15.0) {
    std::call_once(g_curl_global_init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    handle_ = curl_easy_init();
}

CurlTransport::~CurlTransport() {
    if (handle_ != nullptr) {
        curl_easy_cleanup(static_cast<CURL*>(handle_));
    }
}

Response CurlTransport::send(const Request& request) {
    CURL* handle = static_cast<CURL*>(handle_);
    if (handle == nullptr) {
        return Response::network_failure("curl_easy_init failed");
    }

    curl_easy_reset(handle);

    Capture capture;
    curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    double timeout = timeout_seconds_;
    if (request.abort_at_unix_seconds != nullptr) {
        const double deadline = request.abort_at_unix_seconds->load();
        if (deadline > 0.0) {
            const double remaining = deadline - wall_clock_seconds();
            if (remaining <= 0.0) {
                return Response::network_failure("shutdown deadline passed");
            }
            timeout = (std::min)(timeout, remaining);
        }
        curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, &progress_callback);
        curl_easy_setopt(handle, CURLOPT_XFERINFODATA,
                         const_cast<std::atomic<double>*>(request.abort_at_unix_seconds));
        curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
    }
    const long timeout_ms = (std::max)(1L, static_cast<long>(timeout * 1000.0));
    curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &write_callback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &capture);
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, &header_callback);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, &capture);
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "ravensight-cpp/" RAVENSIGHT_VERSION);

    curl_slist* headers = nullptr;
    for (const auto& header : request.headers) {
        const std::string line = header.first + ": " + header.second;
        headers = curl_slist_append(headers, line.c_str());
    }
    if (headers != nullptr) {
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    }

    if (request.method == "POST") {
        curl_easy_setopt(handle, CURLOPT_POST, 1L);
        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request.body.c_str());
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(request.body.size()));
    } else if (request.method != "GET") {
        curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, request.method.c_str());
    }

    const CURLcode code = curl_easy_perform(handle);
    if (headers != nullptr) {
        curl_slist_free_all(headers);
    }

    if (code != CURLE_OK) {
        return Response::network_failure(curl_easy_strerror(code));
    }

    long status = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);

    Response response;
    response.status = status;
    response.body = std::move(capture.body);
    response.retry_after = std::move(capture.retry_after);
    return response;
}

} // namespace ravensight
