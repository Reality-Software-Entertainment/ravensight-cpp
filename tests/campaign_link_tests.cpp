// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Campaign links: Client::set_campaign_link, the rs_link reader a web build
// uses on its page address, and the once rule on POST /session, against a
// fake server. The same contract as ravensight-js.

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "campaign_link.hpp"
#include "ravensight/client.hpp"
#include "storage.hpp"
#include "temp_dirs.hpp"
#include "test_runner.hpp"

using namespace ravensight;

namespace {

const std::string kLink = "64b0000000000000000000a1";
const std::string kOther = "0123456789abcdef01234567";

std::string upper(std::string text) {
    for (char& ch : text) {
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
    }
    return text;
}

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

Response reply(long status, const std::string& body) {
    Response response;
    response.status = status;
    response.body = body;
    return response;
}

/// Answers sessions S1, S2, ... with 201 unless `on_session` says otherwise,
/// and the first `batch_401s` batches with 401 (so the client opens a new
/// session). `hold_sessions` parks every session request until released.
struct LinkServer {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<Request> requests;
    int sessions = 0;
    int batch_401s = 0;
    bool hold_sessions = false;
    std::function<Response(const Request&, int)> on_session;

    Response handle(const Request& request) {
        std::unique_lock<std::mutex> lock(mutex);
        requests.push_back(request);
        if (ends_with(request.url, "/settings")) return reply(200, "{\"trackingEnabled\":true}");
        if (ends_with(request.url, "/session")) {
            cv.wait_for(lock, std::chrono::seconds(5), [&] { return !hold_sessions; });
            const int index = sessions++;
            if (on_session) {
                std::function<Response(const Request&, int)> answer = on_session;
                lock.unlock();
                const Response scripted = answer(request, index);
                if (scripted.status != -1) return scripted;
            }
            return reply(201, "{\"token\":\"S" + std::to_string(index + 1) + "\",\"expiresIn\":3600}");
        }
        if (request.url.find("/ingest/config") != std::string::npos) return reply(304, "");
        if (ends_with(request.url, "/track/batch")) {
            if (batch_401s > 0) {
                --batch_401s;
                return reply(401, "{\"error\":\"invalid_session\"}");
            }
            return reply(202, "{\"accepted\":1}");
        }
        return reply(404, "{}");
    }

    void release_sessions() {
        std::lock_guard<std::mutex> lock(mutex);
        hold_sessions = false;
        cv.notify_all();
    }

    void add_batch_401s(int count) {
        std::lock_guard<std::mutex> lock(mutex);
        batch_401s += count;
    }

    std::vector<Request> session_requests() {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<Request> out;
        for (const Request& request : requests) {
            if (ends_with(request.url, "/session")) out.push_back(request);
        }
        return out;
    }

    /// The campaignLink of each POST /session so far, "" where none was sent.
    std::vector<std::string> links() {
        std::vector<std::string> out;
        for (const Request& request : session_requests()) {
            const json::Value body = json::Value::parse(request.body);
            const json::Value* link = body.find("campaignLink");
            out.push_back(link != nullptr && link->is_string() ? link->as_string() : "");
        }
        return out;
    }

    std::size_t total() {
        std::lock_guard<std::mutex> lock(mutex);
        return requests.size();
    }
};

class Wire : public Transport {
public:
    explicit Wire(std::shared_ptr<LinkServer> server) : server_(std::move(server)) {}
    Response send(const Request& request) override { return server_->handle(request); }

private:
    std::shared_ptr<LinkServer> server_;
};

Config link_config(const std::string& dir) {
    Config config;
    config.ingest_key = "gt_pub_campaign";
    config.api_url = "https://api.example.test";
    config.platform = "test";
    config.storage_dir = dir;
    config.flush_interval_seconds = 0;
    config.track_lifecycle_events = false;
    config.error_capture = false;
    config.persist_queue = true;
    config.log = [](const std::string&) {};
    return config;
}

bool wait_for(const std::function<bool()>& done, double seconds = 3.0) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < until) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(120)); }

/// Sends one event, opening a session first when none is held.
bool send_one(Client& client) {
    client.track("tick");
    return client.flush_and_wait(3.0);
}

using Links = std::vector<std::string>;

/// True when no file under `dir` mentions the link id.
bool nothing_saved(const std::string& dir, const std::string& link) {
    std::error_code error;
    if (!std::filesystem::exists(dir, error)) return true;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, error)) {
        if (!entry.is_regular_file()) continue;
        std::string text;
        detail::read_text_file(entry.path().string(), text);
        if (text.find(link) != std::string::npos || text.find(upper(link)) != std::string::npos) return false;
    }
    return true;
}

} // namespace

// --- The id check ------------------------------------------------------------------

RS_TEST(campaign_link_id_accepts_24_hex_in_any_case_and_lower_cases_it) {
    CHECK_EQ(detail::campaign_link_id(kLink), kLink);
    CHECK_EQ(detail::campaign_link_id(upper(kLink)), kLink);
    CHECK_EQ(detail::campaign_link_id("ABCDEFabcdef0123456789Ab"), std::string("abcdefabcdef0123456789ab"));
}

RS_TEST(campaign_link_id_refuses_anything_else) {
    const std::vector<std::string> refused = {
        "",
        kLink.substr(0, 23),
        kLink + "0",
        std::string(24, 'g'),
        " " + kLink.substr(1),
        kLink.substr(1) + " ",
        " " + kLink,
        kLink + " ",
        "0x" + kLink.substr(2),
        kLink.substr(0, 12) + "-" + kLink.substr(13),
        std::string("64b000000000000000000\0a1", 24),
    };
    for (const std::string& text : refused) {
        CHECK_EQ(detail::campaign_link_id(text), std::string(""));
    }
}

// --- The page address (web builds) ------------------------------------------------

RS_TEST(campaign_link_from_search_reads_the_first_rs_link_as_url_search_params_does) {
    struct Case {
        std::string search;
        std::string expected;
    };
    const std::vector<Case> cases = {
        {"?lang=en&rs_link=" + upper(kLink) + "&utm_source=x", kLink},
        {"?rs_link=abc", ""},
        {"?rs_link=" + std::string(24, 'g'), ""},
        {"?ref=x", ""},
        {"", ""},
        {"?rs_link=" + kLink + "&rs_link=" + kOther, kLink},
        // location.search never includes the fragment.
        {"?a=1&rs_link=" + kLink + "#frag", kLink},
        {"?rs_link=%36%34b0000000000000000000a1", kLink},
        // The first rs_link decides, even when it is not a link id.
        {"?rs_link=abc&rs_link=" + kLink, ""},
        {"?rs_link=&rs_link=" + kLink, ""},
        // Without the leading '?', as URLSearchParams also accepts.
        {"rs_link=" + kLink, kLink},
        // Only one leading '?' is dropped.
        {"??rs_link=" + kLink, ""},
        {"?&&rs_link=" + kLink + "&&", kLink},
        // The name is decoded too, and must match exactly.
        {"?rs%5Flink=" + kLink, kLink},
        {"?RS_LINK=" + kLink, ""},
        {"?rs_link2=" + kLink, ""},
        {"?xrs_link=" + kLink, ""},
        // '+' is a space, and a space makes it no id.
        {"?rs_link=+" + kLink, ""},
        {"?rs_link=" + kLink + "%20", ""},
        {"?rs_link=" + kLink + "+", ""},
        // A value with '=' in it keeps everything after the first '='.
        {"?rs_link=" + kLink + "=", ""},
        {"?rs_link", ""},
        // A broken escape stays as it is.
        {"?rs_link=%zz" + kLink.substr(3), ""},
        {"?rs_link=" + kLink + "%", ""},
        {"?#rs_link=" + kLink, ""},
    };
    for (const Case& test : cases) {
        const std::string got = detail::campaign_link_from_search(test.search);
        if (!CHECK_EQ(got, test.expected)) {
            std::printf("    search: %s\n", test.search.c_str());
        }
    }
}

RS_TEST(campaign_link_page_search_is_empty_on_a_build_that_is_not_a_web_build) {
#if !defined(__EMSCRIPTEN__)
    CHECK_EQ(detail::page_search(), std::string(""));
#endif
}

// --- set_campaign_link and the once rule ------------------------------------------

RS_TEST(campaign_link_goes_lower_cased_on_the_first_session_and_never_again) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<LinkServer>();
    server->batch_401s = 1; // the first batch's session is refused: a second session opens
    Client client(link_config(dir), std::make_unique<Wire>(server));
    CHECK(client.set_campaign_link(upper(kLink)));
    CHECK(send_one(client));
    CHECK(server->links() == (Links{kLink, ""}));
    // A third session (expiry, a new device id) carries none either.
    client.reset_device_id();
    CHECK(send_one(client));
    CHECK(server->links() == (Links{kLink, "", ""}));
    CHECK(nothing_saved(dir, kLink));
}

RS_TEST(campaign_link_set_returns_false_for_anything_but_a_link_id_and_keeps_the_waiting_one) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<LinkServer>();
    Client client(link_config(dir), std::make_unique<Wire>(server));
    CHECK(client.set_campaign_link(kLink));
    for (const std::string& text : {std::string(""), kLink.substr(0, 23), kLink + "0", std::string(24, 'g'),
                                    " " + kLink, kLink + " ", " " + kLink.substr(1)}) {
        CHECK(!client.set_campaign_link(text));
    }
    CHECK(send_one(client));
    CHECK(server->links() == (Links{kLink}));
}

RS_TEST(campaign_link_nothing_is_sent_when_every_set_was_refused) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<LinkServer>();
    Client client(link_config(dir), std::make_unique<Wire>(server));
    CHECK(!client.set_campaign_link("abc"));
    CHECK(!client.set_campaign_link(std::string(24, 'g')));
    CHECK(send_one(client));
    CHECK(server->links() == (Links{""}));
    // No campaignLink key at all, not an empty one.
    CHECK(json::Value::parse(server->session_requests()[0].body).find("campaignLink") == nullptr);
}

RS_TEST(campaign_link_a_later_valid_set_replaces_the_waiting_one) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<LinkServer>();
    Client client(link_config(dir), std::make_unique<Wire>(server));
    CHECK(client.set_campaign_link(kLink));
    CHECK(client.set_campaign_link(upper(kOther)));
    CHECK(send_one(client));
    CHECK(server->links() == (Links{kOther}));
}

RS_TEST(campaign_link_set_after_the_first_session_goes_with_the_next_session) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<LinkServer>();
    Client client(link_config(dir), std::make_unique<Wire>(server));
    CHECK(send_one(client));
    CHECK(client.set_campaign_link(kLink));
    server->add_batch_401s(1);
    CHECK(send_one(client));
    CHECK(server->links() == (Links{"", kLink}));
}

RS_TEST(campaign_link_a_failed_or_refused_session_keeps_it_for_the_next_attempt) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<LinkServer>();
    server->on_session = [](const Request&, int index) {
        if (index == 0) return reply(500, "{\"error\":\"internal_error\"}");
        if (index == 1) return reply(0, "");                       // no answer (timed out)
        if (index == 2) return reply(429, "{\"error\":\"rate_limited\"}");
        if (index == 3) return reply(201, "{\"expiresIn\":3600}"); // 201 with no token
        return reply(-1, "");
    };
    server->batch_401s = 1;
    Client client(link_config(dir), std::make_unique<Wire>(server));
    CHECK(client.set_campaign_link(kLink));
    for (int attempt = 0; attempt < 6 && server->links().size() < 6; ++attempt) {
        send_one(client);
    }
    // Four failures carry it, the accepted fifth carries it and clears it, the sixth has none.
    CHECK(server->links() == (Links{kLink, kLink, kLink, kLink, kLink, ""}));
}

RS_TEST(campaign_link_set_while_a_session_is_in_flight_waits_for_the_next_session) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<LinkServer>();
    server->hold_sessions = true;
    server->batch_401s = 1;
    Client client(link_config(dir), std::make_unique<Wire>(server));
    CHECK(client.set_campaign_link(kLink));
    client.track("tick");
    client.flush();
    REQUIRE(wait_for([&] { return server->session_requests().size() == 1; }));
    // The first session already carries kLink; kOther is for the next one.
    CHECK(client.set_campaign_link(kOther));
    server->release_sessions();
    REQUIRE(wait_for([&] { return server->session_requests().size() == 2; }));
    CHECK(server->links() == (Links{kLink, kOther}));
}

RS_TEST(campaign_link_the_same_link_set_again_while_in_flight_goes_once) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<LinkServer>();
    server->hold_sessions = true;
    server->batch_401s = 1;
    Client client(link_config(dir), std::make_unique<Wire>(server));
    CHECK(client.set_campaign_link(kLink));
    client.track("tick");
    client.flush();
    REQUIRE(wait_for([&] { return server->session_requests().size() == 1; }));
    // Changed and changed back while the open session already carries it.
    CHECK(client.set_campaign_link(kOther));
    CHECK(client.set_campaign_link(upper(kLink)));
    server->release_sessions();
    REQUIRE(wait_for([&] { return server->session_requests().size() == 2; }));
    CHECK(server->links() == (Links{kLink, ""}));
}

// --- Gates ----------------------------------------------------------------------------

RS_TEST(campaign_link_waits_for_consent_and_goes_with_the_first_session_after_the_yes) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<LinkServer>();
    Config config = link_config(dir);
    config.consent = Consent::opt_in;
    Client client(config, std::make_unique<Wire>(server));
    CHECK(client.set_campaign_link(kLink));
    client.track("tick");
    client.flush();
    settle();
    CHECK_EQ(server->total(), static_cast<std::size_t>(0));
    client.set_tracking_enabled(true);
    CHECK(send_one(client));
    CHECK(server->links() == (Links{kLink}));
    CHECK(nothing_saved(dir, kLink));
}

RS_TEST(campaign_link_an_opted_out_player_sends_nothing) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<LinkServer>();
    Config config = link_config(dir);
    REQUIRE(set_tracking_enabled(config, false));
    Client client(config, std::make_unique<Wire>(server));
    CHECK(client.set_campaign_link(kLink));
    client.track("tick");
    client.flush();
    settle();
    CHECK(server->session_requests().empty());
    CHECK(nothing_saved(dir, kLink));
}

RS_TEST(campaign_link_waits_while_the_game_switched_sending_off) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<LinkServer>();
    Config config = link_config(dir);
    config.enabled = false;
    Client client(config, std::make_unique<Wire>(server));
    CHECK(client.set_campaign_link(kLink));
    client.track("tick");
    client.flush();
    settle();
    CHECK(server->session_requests().empty());
    client.set_enabled(true);
    CHECK(send_one(client));
    CHECK(server->links() == (Links{kLink}));
}

RS_TEST(campaign_link_without_a_key_nothing_is_sent) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<LinkServer>();
    Config config = link_config(dir);
    config.ingest_key = "";
    config.optional = true;
    Client client(config, std::make_unique<Wire>(server));
    CHECK(client.set_campaign_link(kLink));
    client.track("tick");
    client.flush_and_wait(1.0);
    settle();
    CHECK_EQ(server->total(), static_cast<std::size_t>(0));
    CHECK(nothing_saved(dir, kLink));
}

RS_TEST(campaign_link_a_playtest_run_never_sends_it) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<LinkServer>();
    server->batch_401s = 1;
    Config config = link_config(dir);
    config.playtest_token = "pt_token";
    config.playtest_run_id = "run1";
    Client client(config, std::make_unique<Wire>(server));
    CHECK(client.set_campaign_link(kLink));
    CHECK(send_one(client));
    CHECK(server->links() == (Links{"", ""}));
}

RS_TEST(campaign_link_a_join_token_session_never_carries_it_and_does_not_clear_it) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<LinkServer>();
    server->batch_401s = 1; // the joined session is refused later: a plain one opens
    Config config = link_config(dir);
    config.join_token = "st_from_player";
    Client client(config, std::make_unique<Wire>(server));
    CHECK(client.set_campaign_link(kLink));
    CHECK(send_one(client));
    const std::vector<Request> sessions = server->session_requests();
    REQUIRE(sessions.size() == 2u);
    CHECK_EQ(json::Value::parse(sessions[0].body).get_string("joinToken", ""), std::string("st_from_player"));
    CHECK(json::Value::parse(sessions[1].body).find("joinToken") == nullptr);
    CHECK(server->links() == (Links{"", kLink}));
}

RS_TEST(campaign_link_a_refused_join_token_falls_back_to_a_plain_session_that_carries_it) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<LinkServer>();
    server->on_session = [](const Request& request, int) {
        if (json::Value::parse(request.body).find("joinToken") != nullptr) {
            return reply(401, "{\"error\":\"invalid_join_token\"}");
        }
        return reply(-1, "");
    };
    Config config = link_config(dir);
    config.join_token = "stale";
    Client client(config, std::make_unique<Wire>(server));
    CHECK(client.set_campaign_link(kLink));
    CHECK(send_one(client));
    CHECK(server->links() == (Links{"", kLink}));
}
