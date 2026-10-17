// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// The typed event helpers: each builds the reserved rs_* properties and
// calls Client::track, nothing else.

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

#include "ravensight/client.hpp"

namespace ravensight {

namespace {

constexpr std::size_t max_message_chars = 200;
constexpr std::size_t max_stack_chars = 2000;

/// The caller's extra properties first, minus any key the helper sets, then
/// the reserved ones. So a reserved key always wins.
EventData merged(const EventData& extra, EventData reserved) {
    EventData out;
    out.reserve(extra.size() + reserved.size());
    for (const auto& entry : extra) {
        const bool shadowed = std::any_of(reserved.begin(), reserved.end(),
                                          [&](const auto& r) { return r.first == entry.first; });
        if (!shadowed) {
            out.push_back(entry);
        }
    }
    for (auto& entry : reserved) {
        out.push_back(std::move(entry));
    }
    return out;
}

std::string upper_ascii(std::string text) {
    for (char& c : text) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return text;
}

/// Cuts to at most max_chars characters (UTF-8 code points, so a cut never
/// leaves half a character).
std::string cut_chars(const std::string& text, std::size_t max_chars) {
    std::size_t chars = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
            if (chars == max_chars) {
                return text.substr(0, i);
            }
            ++chars;
        }
    }
    return text;
}

void add_if_set(EventData& data, const char* key, const std::string& value) {
    if (!value.empty()) {
        data.emplace_back(key, value);
    }
}

} // namespace

bool Client::track_purchase(const Field& amount, const std::string& currency,
                            const std::string& item_type, const std::string& item_id,
                            const std::string& cart, const std::string& transaction_id,
                            const EventData& extra, const std::string& event_name) {
    EventData reserved{{"rs_kind", "purchase"},
                       {"rs_amount", amount},
                       {"rs_currency", upper_ascii(currency)},
                       {"rs_item_type", item_type},
                       {"rs_item_id", item_id}};
    add_if_set(reserved, "rs_cart", cart);
    add_if_set(reserved, "rs_transaction_id", transaction_id);
    return track(event_name.empty() ? "purchase" : event_name, merged(extra, std::move(reserved)));
}

bool Client::track_resource(const std::string& flow, const std::string& resource, double amount,
                            const std::string& item_type, const std::string& item_id,
                            const EventData& extra, const std::string& event_name) {
    EventData reserved{{"rs_kind", "resource"},
                       {"rs_flow", flow},
                       {"rs_resource", resource},
                       {"rs_amount", amount}};
    add_if_set(reserved, "rs_item_type", item_type);
    add_if_set(reserved, "rs_item_id", item_id);
    return track(event_name.empty() ? "resource_" + flow : event_name,
                 merged(extra, std::move(reserved)));
}

bool Client::track_progression(const std::string& status, const std::string& p1,
                               const std::string& p2, const std::string& p3,
                               std::optional<double> score, const EventData& extra,
                               const std::string& event_name) {
    EventData reserved{{"rs_kind", "progression"}, {"rs_status", status}, {"rs_p1", p1}};
    add_if_set(reserved, "rs_p2", p2);
    add_if_set(reserved, "rs_p3", p3);
    if (score.has_value()) {
        reserved.emplace_back("rs_score", *score);
    }
    return track(event_name.empty() ? "progression_" + status : event_name,
                 merged(extra, std::move(reserved)));
}

bool Client::track_error(const std::string& severity, const std::string& message,
                         const std::string& stack, const EventData& extra,
                         const std::string& event_name) {
    EventData reserved{{"rs_kind", "error"},
                       {"rs_severity", severity},
                       {"rs_message", cut_chars(message, max_message_chars)}};
    add_if_set(reserved, "rs_stack", cut_chars(stack, max_stack_chars));
    return track(event_name.empty() ? "error" : event_name, merged(extra, std::move(reserved)));
}

} // namespace ravensight
