// Copyright 2026 Reality Software Entertainment. MIT licensed.

#include "campaign_link.hpp"

#include <atomic>
#include <cstddef>

#if defined(__EMSCRIPTEN__)
#include <emscripten/em_asm.h>
#endif

namespace ravensight {
namespace detail {

namespace {

int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/// One name or value of a query string, decoded as
/// application/x-www-form-urlencoded: '+' is a space, then each %XX is
/// its byte, and a '%' without two hex digits after it stays as it is.
std::string form_decode(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '+') {
            out.push_back(' ');
        } else if (ch == '%' && i + 2 < text.size() && hex_value(text[i + 1]) >= 0 &&
                   hex_value(text[i + 2]) >= 0) {
            out.push_back(static_cast<char>(hex_value(text[i + 1]) * 16 + hex_value(text[i + 2])));
            i += 2;
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

#if defined(__EMSCRIPTEN__)
/// Copies the page's location.search into `out` when it fits in `capacity`
/// bytes and returns its length (so a longer one can be read again into a
/// bigger buffer). 0 when there is none or it cannot be read. The address
/// keeps its query percent-encoded, so it is ASCII; anything else is
/// treated as none. Runs on the browser's main thread, where the page is,
/// even when called from a worker thread. Double quotes only in the script:
/// the C preprocessor reads it first.
int read_page_search(char* out, int capacity) {
    return MAIN_THREAD_EM_ASM_INT({
        var search = "";
        try {
            if (typeof location !== "undefined" && location && typeof location.search === "string") {
                search = location.search;
            }
        } catch (e) {
            return 0;
        }
        for (var i = 0; i < search.length; i++) {
            if (search.charCodeAt(i) > 127) return 0;
        }
        if (search.length <= $1) {
            for (var j = 0; j < search.length; j++) HEAPU8[$0 + j] = search.charCodeAt(j);
        }
        return search.length;
    }, out, capacity);
}
#endif

} // namespace

std::string campaign_link_id(const std::string& text) {
    if (text.size() != 24) return "";
    std::string out(text);
    for (char& ch : out) {
        if (hex_value(ch) < 0) return "";
        if (ch >= 'A' && ch <= 'F') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return out;
}

std::string campaign_link_from_search(const std::string& search) {
    const std::string query = search.substr(0, search.find('#'));
    std::size_t start = !query.empty() && query[0] == '?' ? 1 : 0;
    while (start < query.size()) {
        std::size_t end = query.find('&', start);
        if (end == std::string::npos) end = query.size();
        if (end > start) {
            const std::string pair = query.substr(start, end - start);
            const std::size_t equals = pair.find('=');
            if (form_decode(pair.substr(0, equals)) == "rs_link") {
                // The first rs_link decides, as URLSearchParams.get does.
                return equals == std::string::npos ? "" : campaign_link_id(form_decode(pair.substr(equals + 1)));
            }
        }
        start = end + 1;
    }
    return "";
}

std::string page_search() {
#if defined(__EMSCRIPTEN__)
    std::string search(512, '\0');
    int length = read_page_search(&search[0], static_cast<int>(search.size()));
    if (length > static_cast<int>(search.size())) {
        search.assign(static_cast<std::size_t>(length), '\0');
        length = read_page_search(&search[0], length);
        // The address changed between the two reads: no link this time.
        if (length > static_cast<int>(search.size())) return "";
    }
    search.resize(length > 0 ? static_cast<std::size_t>(length) : 0);
    return search;
#else
    return "";
#endif
}

std::string take_page_campaign_link() {
    static std::atomic<bool> taken{false};
    if (taken.exchange(true)) return "";
    return campaign_link_from_search(page_search());
}

} // namespace detail
} // namespace ravensight
