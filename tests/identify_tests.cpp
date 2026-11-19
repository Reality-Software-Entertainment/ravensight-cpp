// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// identify / reset_identity against the binding vectors
// (tests/fixtures/identify-vectors.json, copied from the platform repo's
// docs/identify/identify-vectors.json, version 2) and a fake server whose
// session answers carry, change or leave out identify.salt.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "delivery.hpp"
#include "identify.hpp"
#include "ravensight/client.hpp"
#include "storage.hpp"
#include "temp_dirs.hpp"
#include "test_runner.hpp"

using namespace ravensight;

namespace {

const json::Value& vectors() {
    static const json::Value parsed = [] {
        std::string text;
        detail::read_text_file(std::string(RAVENSIGHT_TEST_FIXTURES) + "/identify-vectors.json", text);
        return json::Value::parse(text);
    }();
    return parsed;
}

std::string test_salt() { return vectors().get_string("salt", ""); }

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string header(const Request& request, const std::string& name) {
    for (const auto& entry : request.headers) {
        if (entry.first == name) return entry.second;
    }
    return "";
}

Response reply(long status, const std::string& body) {
    Response response;
    response.status = status;
    response.body = body;
    return response;
}

/// Numbers its sessions S1, S2, ... across launches (share one Server), and
/// answers each with `salt` (empty: no identify object at all).
struct Server {
    std::mutex mutex;
    std::vector<Request> requests;
    int sessions = 0;
    std::string salt = test_salt();
    std::function<Response(const Request&)> on_identify;

    Response handle(const Request& request) {
        std::function<Response(const Request&)> identify_answer;
        {
            std::lock_guard<std::mutex> lock(mutex);
            requests.push_back(request);
            identify_answer = on_identify;
        }
        if (ends_with(request.url, "/settings")) return reply(200, "{\"trackingEnabled\":true}");
        if (ends_with(request.url, "/session")) {
            std::lock_guard<std::mutex> lock(mutex);
            ++sessions;
            std::string body = "{\"token\":\"S" + std::to_string(sessions) + "\",\"expiresIn\":3600";
            if (!salt.empty()) body += ",\"identify\":{\"salt\":\"" + salt + "\"}";
            return reply(201, body + "}");
        }
        if (ends_with(request.url, "/identify")) {
            if (identify_answer) return identify_answer(request);
            return reply(200, "{\"playerId\":\"px_0\",\"linked\":true,\"verified\":false,\"repeat\":false}");
        }
        if (request.url.find("/ingest/config") != std::string::npos) return reply(304, "");
        if (ends_with(request.url, "/track/batch")) return reply(202, "{\"accepted\":1}");
        return reply(404, "{}");
    }

    /// (session token, accountHash) of every identify request so far.
    std::vector<std::pair<std::string, std::string>> sends() {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::pair<std::string, std::string>> out;
        for (const Request& request : requests) {
            if (!ends_with(request.url, "/identify")) continue;
            out.emplace_back(header(request, "X-Session-Token"),
                             json::Value::parse(request.body).get_string("accountHash", ""));
        }
        return out;
    }

    std::size_t count(const std::string& suffix) {
        std::lock_guard<std::mutex> lock(mutex);
        std::size_t n = 0;
        for (const Request& request : requests) {
            if (ends_with(request.url, suffix)) ++n;
        }
        return n;
    }

    int session_count() {
        std::lock_guard<std::mutex> lock(mutex);
        return sessions;
    }
};

class Wire : public Transport {
public:
    explicit Wire(std::shared_ptr<Server> server) : server_(std::move(server)) {}
    Response send(const Request& request) override { return server_->handle(request); }

private:
    std::shared_ptr<Server> server_;
};

Config identify_config(const std::string& dir) {
    Config config;
    config.ingest_key = "gt_pub_identify";
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

/// Lets the worker run whatever it has queued, then reports quiet.
void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(120)); }

/// Opens (or reuses) a session by sending one event.
void open_session(Client& client) {
    client.track("tick");
    client.flush_and_wait(3.0);
}

std::string identity_file(const std::string& dir) { return detail::join_path(dir, "identity"); }

/// The stored {salt, accountHash}, or a null Value when nothing is stored.
json::Value stored(const std::string& dir) {
    std::string text;
    if (!detail::read_text_file(identity_file(dir), text)) return json::Value();
    return json::Value::parse(text);
}

const json::Value& step(std::size_t index, const std::string& starts) {
    const json::Value& entry = vectors().find("sequence")->at(index);
    CHECK(entry.get_string("step", "").rfind(starts, 0) == 0);
    return entry;
}

std::string expected_send(const json::Value& entry) {
    return entry.find("expect")->find("sends")->at(0).get_string("accountHash", "");
}

std::string expected_stored(const json::Value& entry) {
    const json::Value* value = entry.find("expect")->find("stored");
    return value != nullptr && value->is_object() ? value->get_string("accountHash", "") : "";
}

} // namespace

// --- 1. accept vectors ---------------------------------------------------------

RS_TEST(identify_vectors_are_version_2) {
    double version = 0;
    REQUIRE(vectors().get_number("version", version));
    CHECK_EQ(version, 2.0);
    CHECK_EQ(test_salt().size(), static_cast<std::size_t>(64));
}

RS_TEST(identify_every_accept_vector_hashes_to_its_wire) {
    int checked = 0;
    for (const json::Value& entry : vectors().find("accept")->items()) {
        const json::Value* id = entry.find("accountId");
        if (!id->is_string()) continue; // the integer case: C++ takes std::string only
        CHECK(detail::identify_refusal(id->as_string()) == nullptr);
        CHECK_EQ(detail::identify_account_hash(test_salt(), id->as_string()), entry.get_string("wire", ""));
        CHECK_EQ("px_" + entry.get_string("wire", "").substr(0, 32), entry.get_string("playerId", ""));
        ++checked;
    }
    CHECK_EQ(checked, 10);
    const json::Value* differ = vectors().find("salts_differ");
    REQUIRE(differ != nullptr);
    const std::string account = differ->get_string("accountId", "");
    CHECK_EQ(detail::identify_account_hash(differ->get_string("salt_a", ""), account), differ->get_string("wire_a", ""));
    CHECK_EQ(detail::identify_account_hash(differ->get_string("salt_b", ""), account), differ->get_string("wire_b", ""));
}

RS_TEST(identify_every_accept_vector_reaches_the_wire_through_a_client) {
    for (const json::Value& entry : vectors().find("accept")->items()) {
        const json::Value* id = entry.find("accountId");
        if (!id->is_string()) continue;
        const std::string dir = rstest::make_temp_dir();
        auto server = std::make_shared<Server>();
        Client client(identify_config(dir), std::make_unique<Wire>(server));
        CHECK(client.identify(id->as_string()));
        open_session(client);
        REQUIRE(wait_for([&] { return server->sends().size() == 1; }));
        CHECK_EQ(server->sends()[0].second, entry.get_string("wire", ""));
        CHECK_EQ(stored(dir).get_string("accountHash", ""), entry.get_string("wire", ""));
    }
}

// --- 2. refuse vectors -----------------------------------------------------------

RS_TEST(identify_every_refuse_vector_is_refused_with_nothing_sent_or_stored) {
    int checked = 0;
    for (const json::Value& entry : vectors().find("refuse")->items()) {
        const json::Value* id = entry.find("accountId");
        // null and a fraction cannot be passed as std::string.
        if (!id->is_string()) continue;
        const char* reason = detail::identify_refusal(id->as_string());
        REQUIRE(reason != nullptr);
        CHECK_EQ(std::string(reason), entry.get_string("reason", ""));

        const std::string dir = rstest::make_temp_dir();
        auto server = std::make_shared<Server>();
        std::vector<std::string> lines;
        Config config = identify_config(dir);
        config.log = [&lines](const std::string& line) { lines.push_back(line); };
        Client client(config, std::make_unique<Wire>(server));
        open_session(client);
        const std::string device = client.device_id();
        CHECK(!client.identify(id->as_string()));
        CHECK(!client.is_identified());
        open_session(client);
        settle();
        CHECK(server->sends().empty());
        CHECK(stored(dir).is_null());
        CHECK_EQ(client.device_id(), device);
        ++checked;
    }
    CHECK_EQ(checked, 7);
}

RS_TEST(identify_refusal_keeps_what_is_stored) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<Server>();
    Client client(identify_config(dir), std::make_unique<Wire>(server));
    CHECK(client.identify("u1"));
    open_session(client);
    REQUIRE(wait_for([&] { return server->sends().size() == 1; }));
    const std::string before = stored(dir).dump();
    CHECK(!client.identify(std::string(257, 'a')));
    CHECK(!client.identify(""));
    CHECK_EQ(stored(dir).dump(), before);
    CHECK(client.is_identified());
}

// --- 3. the sequence ---------------------------------------------------------------

RS_TEST(identify_sequence_holds) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<Server>();
    const Config config = identify_config(dir);

    std::string device;
    {
        Client client(config, std::make_unique<Wire>(server));
        // 0: identify before any session: held in memory only.
        const json::Value& s0 = step(0, "init with a key and consent");
        (void)s0;
        CHECK(client.identify("u1"));
        CHECK(client.is_identified());
        settle();
        CHECK(server->sends().empty());
        CHECK(stored(dir).is_null());

        // 1: S1 opens with the salt: hashed, stored, sent on S1.
        const json::Value& s1 = step(1, "session S1 opens");
        open_session(client);
        REQUIRE(wait_for([&] { return server->sends().size() == 1; }));
        CHECK_EQ(server->sends()[0].first, std::string("S1"));
        CHECK_EQ(server->sends()[0].second, expected_send(s1));
        CHECK_EQ(stored(dir).get_string("accountHash", ""), expected_stored(s1));
        CHECK_EQ(stored(dir).get_string("salt", ""), test_salt());

        // 2: the same account again in S1: nothing.
        step(2, "identify(\"u1\") again in S1");
        device = client.device_id();
        CHECK(client.identify("u1"));
        open_session(client);
        settle();
        CHECK_EQ(server->sends().size(), static_cast<std::size_t>(1));
        CHECK_EQ(client.device_id(), device);
    }
    {
        // 3: relaunch, S2 opens with the same salt, no call: sent on S2.
        const json::Value& s3 = step(3, "relaunch, session S2 opens");
        Client client(config, std::make_unique<Wire>(server));
        CHECK(client.is_identified());
        open_session(client);
        REQUIRE(wait_for([&] { return server->sends().size() == 2; }));
        CHECK_EQ(server->sends()[1].first, std::string("S2"));
        CHECK_EQ(server->sends()[1].second, expected_send(s3));
        CHECK_EQ(client.device_id(), device);

        // 4: an email is refused; nothing changes.
        const json::Value& s4 = step(4, "identify(\"player@example.com\")");
        CHECK(!client.identify("player@example.com"));
        settle();
        CHECK_EQ(server->sends().size(), static_cast<std::size_t>(2));
        CHECK_EQ(stored(dir).get_string("accountHash", ""), expected_stored(s4));
        CHECK_EQ(client.device_id(), device);

        // 5: another account: fresh device and session S3, sent on S3.
        const json::Value& s5 = step(5, "identify(\"u2\")");
        CHECK(client.identify("u2"));
        CHECK(client.device_id() != device);
        CHECK_EQ(stored(dir).get_string("accountHash", ""), expected_stored(s5));
        open_session(client);
        REQUIRE(wait_for([&] { return server->sends().size() == 3; }));
        CHECK_EQ(server->sends()[2].first, std::string("S3"));
        CHECK_EQ(server->sends()[2].second, expected_send(s5));

        // 6: reset_identity: fresh device, S4, nothing sent, nothing stored.
        step(6, "resetIdentity()");
        const std::string before_reset = client.device_id();
        client.reset_identity();
        CHECK(client.device_id() != before_reset);
        CHECK(!client.is_identified());
        CHECK(stored(dir).is_null());
        open_session(client);
        settle();
        CHECK_EQ(server->session_count(), 4);
        CHECK_EQ(server->sends().size(), static_cast<std::size_t>(3));

        // 7: reset again with nothing stored: nothing at all.
        step(7, "resetIdentity() again");
        const std::string kept = client.device_id();
        client.reset_identity();
        CHECK_EQ(client.device_id(), kept);
        open_session(client);
        settle();
        CHECK_EQ(server->session_count(), 4);
        CHECK_EQ(server->sends().size(), static_cast<std::size_t>(3));

        // 8: u3 answered 429 needs_funds: stored, sent once on S4, no loop.
        const json::Value& s8 = step(8, "identify(\"u3\")");
        {
            std::lock_guard<std::mutex> lock(server->mutex);
            server->on_identify = [](const Request&) {
                return reply(429, "{\"error\":\"quota_exceeded\",\"reason\":\"needs_funds\"}");
            };
        }
        CHECK(client.identify("u3"));
        REQUIRE(wait_for([&] { return server->sends().size() == 4; }));
        CHECK_EQ(server->sends()[3].first, std::string("S4"));
        CHECK_EQ(server->sends()[3].second, expected_send(s8));
        CHECK_EQ(stored(dir).get_string("accountHash", ""), expected_stored(s8));
        open_session(client);
        client.flush();
        settle();
        CHECK_EQ(server->sends().size(), static_cast<std::size_t>(4));
    }
    {
        // 9: relaunch under another salt: the stored identity is deleted, never sent.
        step(9, "relaunch; session S5 answers a DIFFERENT salt");
        {
            std::lock_guard<std::mutex> lock(server->mutex);
            server->on_identify = nullptr;
            server->salt = vectors().find("salts_differ")->get_string("salt_b", "");
        }
        Client client(config, std::make_unique<Wire>(server));
        open_session(client);
        settle();
        CHECK_EQ(server->sends().size(), static_cast<std::size_t>(4));
        CHECK(stored(dir).is_null());
        CHECK(!client.is_identified());
    }
    {
        // 10: an answer with no salt: unavailable this launch, the held id dropped.
        step(10, "identify(\"u4\")");
        {
            std::lock_guard<std::mutex> lock(server->mutex);
            server->salt.clear();
        }
        Client client(config, std::make_unique<Wire>(server));
        CHECK(client.identify("u4"));
        open_session(client);
        settle();
        CHECK(!client.is_identified());
        CHECK_EQ(server->sends().size(), static_cast<std::size_t>(4));
        CHECK(stored(dir).is_null());
        CHECK(client.identify("u4"));
        open_session(client);
        settle();
        CHECK_EQ(server->sends().size(), static_cast<std::size_t>(4));
        CHECK(stored(dir).is_null());
    }
    {
        // 11: opt out deletes the stored identity.
        step(11, "opt out");
        {
            std::lock_guard<std::mutex> lock(server->mutex);
            server->salt = test_salt();
        }
        Client client(config, std::make_unique<Wire>(server));
        CHECK(client.identify("u1"));
        open_session(client);
        REQUIRE(wait_for([&] { return server->sends().size() == 5; }));
        CHECK(!stored(dir).is_null());
        // The player's opt-out (consent withdrawn). The game's own switch,
        // set_enabled(false), keeps it: a settled cross-SDK decision.
        client.set_tracking_enabled(false);
        CHECK(stored(dir).is_null());
        CHECK(!client.is_identified());
        client.set_tracking_enabled(true);
        open_session(client);
        settle();
        CHECK_EQ(server->sends().size(), static_cast<std::size_t>(5));
    }
}

RS_TEST(identify_needs_funds_is_retried_at_the_next_session) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<Server>();
    std::atomic<int> answered{0};
    server->on_identify = [&answered](const Request&) {
        return ++answered == 1 ? reply(429, "{\"error\":\"quota_exceeded\",\"reason\":\"needs_funds\"}")
                               : reply(200, "{\"playerId\":\"px_0\",\"linked\":true,\"verified\":false,\"repeat\":false}");
    };
    Client client(identify_config(dir), std::make_unique<Wire>(server));
    CHECK(client.identify("u3"));
    open_session(client);
    REQUIRE(wait_for([&] { return server->sends().size() == 1; }));
    open_session(client);
    settle();
    CHECK_EQ(server->sends().size(), static_cast<std::size_t>(1));
    // reset_device_id keeps the identity: the next session identifies again.
    client.reset_device_id();
    CHECK(client.is_identified());
    open_session(client);
    REQUIRE(wait_for([&] { return server->sends().size() == 2; }));
    CHECK_EQ(server->sends()[1].first, std::string("S2"));
    CHECK_EQ(server->sends()[1].second, server->sends()[0].second);
}

RS_TEST(identify_other_answers_follow_the_table) {
    struct Case {
        long status;
        std::string body;
        bool next_session_retries;
    };
    const Case cases[] = {
        {400, "{\"error\":\"invalid_account_hash\"}", false},
        {400, "{\"error\":\"no_session\"}", true},
        {404, "{}", false},
        {409, "{\"error\":\"session_taken\"}", true},
        {409, "{\"error\":\"synthetic_session\"}", true},
        {429, "{\"error\":\"identify_limited\"}", true},
        {429, "{\"error\":\"rate_limited\"}", true},
        {503, "{}", true},
        {0, "", true},
    };
    for (const Case& c : cases) {
        const std::string dir = rstest::make_temp_dir();
        auto server = std::make_shared<Server>();
        server->on_identify = [c](const Request&) { return reply(c.status, c.body); };
        Client client(identify_config(dir), std::make_unique<Wire>(server));
        CHECK(client.identify("u1"));
        open_session(client);
        REQUIRE(wait_for([&] { return server->sends().size() == 1; }));
        open_session(client);
        settle();
        CHECK_EQ(server->sends().size(), static_cast<std::size_t>(1)); // never in a loop
        CHECK(!stored(dir).is_null()); // the stored identity is kept
        client.reset_device_id();
        open_session(client);
        settle();
        CHECK_EQ(server->sends().size(), static_cast<std::size_t>(c.next_session_retries ? 2 : 1));
    }
}

// --- 4. the request ----------------------------------------------------------------

RS_TEST(identify_request_carries_the_key_the_session_and_only_the_hash) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<Server>();
    Client client(identify_config(dir), std::make_unique<Wire>(server));
    open_session(client);
    CHECK(client.identify("12345")); // in an open session with the salt known: at once
    REQUIRE(wait_for([&] { return server->count("/identify") == 1; }));
    Request request;
    {
        std::lock_guard<std::mutex> lock(server->mutex);
        for (const Request& r : server->requests) {
            if (ends_with(r.url, "/identify")) request = r;
        }
    }
    CHECK_EQ(request.method, std::string("POST"));
    CHECK_EQ(request.url, std::string("https://api.example.test/api/v1/identify"));
    CHECK_EQ(header(request, "X-API-Key"), std::string("gt_pub_identify"));
    CHECK_EQ(header(request, "X-Session-Token"), std::string("S1"));
    const std::string wire = vectors().find("accept")->at(0).get_string("wire", "");
    CHECK_EQ(request.body, "{\"accountHash\":\"" + wire + "\"}");
    CHECK(request.body.find("deviceId") == std::string::npos);
    CHECK(request.body.find(client.device_id()) == std::string::npos);
}

RS_TEST(identify_never_enters_the_event_queue_or_its_file) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<Server>();
    server->on_identify = [](const Request&) { return reply(0, ""); };
    Config config = identify_config(dir);
    Client client(config, std::make_unique<Wire>(server));
    CHECK(client.identify("u1"));
    open_session(client);
    REQUIRE(wait_for([&] { return server->sends().size() == 1; }));
    const std::string hash = server->sends()[0].second;
    CHECK_EQ(client.stats().queued, 0);
    // Batches go out while identify fails.
    CHECK(client.track("after"));
    CHECK(client.flush_and_wait(3.0));
    std::string queue_text;
    detail::read_text_file(detail::join_path(dir, detail::queue_file_name(config.ingest_key)), queue_text);
    CHECK(queue_text.find(hash) == std::string::npos);
    CHECK(queue_text.find("u1") == std::string::npos);
    std::string identity_text;
    REQUIRE(detail::read_text_file(identity_file(dir), identity_text));
    CHECK(identity_text.find("u1\"") == std::string::npos); // never the raw id
}

// --- 5. no key, no consent -------------------------------------------------------------

RS_TEST(identify_without_a_key_sends_and_stores_nothing) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<Server>();
    Config config = identify_config(dir);
    config.ingest_key.clear();
    config.optional = true;
    Client client(config, std::make_unique<Wire>(server));
    CHECK(client.identify("u1"));
    CHECK(!client.identify("a@b"));
    client.reset_identity();
    CHECK(client.identify("u1"));
    settle();
    CHECK_EQ(server->count(""), static_cast<std::size_t>(0));
    CHECK(stored(dir).is_null());
}

RS_TEST(identify_before_consent_is_held_then_sent_on_the_yes) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<Server>();
    Config config = identify_config(dir);
    config.consent = Consent::opt_in;
    Client client(config, std::make_unique<Wire>(server));
    CHECK(client.identify("u1"));
    CHECK(client.is_identified());
    client.flush();
    settle();
    CHECK_EQ(server->count(""), static_cast<std::size_t>(0));
    CHECK(stored(dir).is_null());
    client.set_tracking_enabled(true);
    open_session(client);
    REQUIRE(wait_for([&] { return server->sends().size() == 1; }));
    CHECK_EQ(server->sends()[0].second, step(1, "session S1 opens").find("expect")->find("sends")->at(0).get_string("accountHash", ""));
    CHECK(!stored(dir).is_null());
}

RS_TEST(identify_consent_withdrawn_deletes_the_identity_and_the_held_id) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<Server>();
    Client client(identify_config(dir), std::make_unique<Wire>(server));
    CHECK(client.identify("u1"));
    open_session(client);
    REQUIRE(wait_for([&] { return server->sends().size() == 1; }));
    client.set_tracking_enabled(false);
    CHECK(stored(dir).is_null());
    CHECK(!client.is_identified());
    // A held id goes too.
    CHECK(client.identify("u2"));
    CHECK(client.is_identified());
    client.set_tracking_enabled(false);
    CHECK(!client.is_identified());
    client.set_tracking_enabled(true);
    open_session(client);
    settle();
    CHECK_EQ(server->sends().size(), static_cast<std::size_t>(1));
}

RS_TEST(identify_reset_identity_drops_a_held_id_too) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<Server>();
    Client client(identify_config(dir), std::make_unique<Wire>(server));
    const std::string device = client.device_id();
    CHECK(client.identify("u1")); // held: no session yet
    client.reset_identity();
    CHECK(!client.is_identified());
    CHECK(client.device_id() != device);
    open_session(client);
    settle();
    CHECK(server->sends().empty());
}

// --- a late answer -----------------------------------------------------------------------

namespace {

void late_answer_after_reset(long late_status) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<Server>();
    std::mutex gate_mutex;
    std::condition_variable gate;
    bool release = false;
    std::atomic<int> calls{0};
    server->on_identify = [&, late_status](const Request&) {
        if (++calls == 1) {
            std::unique_lock<std::mutex> lock(gate_mutex);
            gate.wait_for(lock, std::chrono::seconds(3), [&] { return release; });
            return reply(late_status, late_status == 200 ? "{\"playerId\":\"px_0\",\"repeat\":false}" : "{}");
        }
        return reply(200, "{\"playerId\":\"px_0\",\"linked\":true,\"verified\":false,\"repeat\":false}");
    };
    Client client(identify_config(dir), std::make_unique<Wire>(server));
    CHECK(client.identify("u1"));
    open_session(client);
    REQUIRE(wait_for([&] { return calls.load() == 1; }));
    // Sign out and in as another account while the u1 answer is in flight.
    client.reset_identity();
    CHECK(client.identify("u2"));
    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        release = true;
    }
    gate.notify_all();
    open_session(client);
    REQUIRE(wait_for([&] { return server->sends().size() == 2; }));
    const auto sends = server->sends();
    CHECK(sends[1].first != sends[0].first);
    CHECK(sends[1].second != sends[0].second);
    CHECK_EQ(stored(dir).get_string("accountHash", ""), sends[1].second);
}

} // namespace

RS_TEST(identify_a_late_200_after_reset_does_not_mark_the_new_session) { late_answer_after_reset(200); }

// A late 404 would stop identify for the launch if it were applied.
RS_TEST(identify_a_late_404_after_reset_changes_nothing) { late_answer_after_reset(404); }

// --- Cross-SDK review findings (F1 to F8) and settled decisions ------------------

RS_TEST(identify_the_game_switch_keeps_the_identity) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<Server>();
    Client client(identify_config(dir), std::make_unique<Wire>(server));
    CHECK(client.identify("u1"));
    open_session(client);
    REQUIRE(wait_for([&] { return server->sends().size() == 1; }));
    client.set_enabled(false);
    CHECK(!stored(dir).is_null());
    CHECK(client.is_identified());
}

RS_TEST(identify_turning_collection_back_on_in_an_open_session_sends) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<Server>();
    Client client(identify_config(dir), std::make_unique<Wire>(server));
    open_session(client);
    client.set_enabled(false);
    CHECK(client.identify("u1")); // held: the client may not save now
    settle();
    CHECK(server->sends().empty());
    CHECK(stored(dir).is_null());
    client.set_enabled(true);
    REQUIRE(wait_for([&] { return server->sends().size() == 1; }));
    CHECK_EQ(server->sends()[0].first, std::string("S1"));
    CHECK(!stored(dir).is_null());
}

RS_TEST(identify_before_the_salt_holds_the_id_and_hashes_under_the_salt_that_arrives) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<Server>();
    const Config config = identify_config(dir);
    {
        Client client(config, std::make_unique<Wire>(server));
        CHECK(client.identify("12345"));
        open_session(client);
        REQUIRE(wait_for([&] { return server->sends().size() == 1; }));
    }
    const json::Value* differ = vectors().find("salts_differ");
    {
        std::lock_guard<std::mutex> lock(server->mutex);
        server->salt = differ->get_string("salt_b", "");
    }
    Client client(config, std::make_unique<Wire>(server));
    const std::string device = client.device_id();
    CHECK(client.identify("12345")); // the saved salt is not this launch's yet
    open_session(client);
    REQUIRE(wait_for([&] { return server->sends().size() == 2; }));
    CHECK_EQ(server->sends()[1].second, differ->get_string("wire_b", ""));
    CHECK_EQ(stored(dir).get_string("salt", ""), differ->get_string("salt_b", ""));
    CHECK_EQ(client.device_id(), device); // the old one was deleted, not signed out
}

RS_TEST(identify_another_account_before_the_salt_signs_the_saved_one_out_when_it_arrives) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<Server>();
    const Config config = identify_config(dir);
    {
        Client client(config, std::make_unique<Wire>(server));
        CHECK(client.identify("u1"));
        open_session(client);
        REQUIRE(wait_for([&] { return server->sends().size() == 1; }));
    }
    Client client(config, std::make_unique<Wire>(server));
    const std::string device = client.device_id();
    CHECK(client.identify("u2"));
    CHECK_EQ(client.device_id(), device); // nothing decided before the salt
    open_session(client); // S2 answers the salt: u1 is signed out first
    CHECK(client.device_id() != device);
    open_session(client); // a fresh session for the fresh device
    REQUIRE(wait_for([&] { return server->sends().size() == 2; }));
    CHECK_EQ(server->sends()[1].first, std::string("S3"));
    CHECK_EQ(server->sends()[1].second, step(5, "identify(\"u2\")").find("expect")->find("sends")->at(0).get_string("accountHash", ""));
}

RS_TEST(identify_two_calls_before_the_salt_the_last_wins) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<Server>();
    Client client(identify_config(dir), std::make_unique<Wire>(server));
    const std::string device = client.device_id();
    CHECK(client.identify("u1"));
    CHECK(client.identify("u2"));
    open_session(client);
    REQUIRE(wait_for([&] { return server->sends().size() == 1; }));
    settle();
    CHECK_EQ(server->sends().size(), static_cast<std::size_t>(1));
    CHECK_EQ(server->sends()[0].second, step(5, "identify(\"u2\")").find("expect")->find("sends")->at(0).get_string("accountHash", ""));
    CHECK_EQ(client.device_id(), device);
}

RS_TEST(identify_never_opens_a_session_under_a_retry_after_hold) {
    std::atomic<bool> refuse{true};
    Config config = identify_config(rstest::make_temp_dir());
    struct Hold : Transport {
        std::shared_ptr<Server> server;
        std::atomic<bool>* refuse;
        Response send(const Request& request) override {
            if (refuse->load() && ends_with(request.url, "/session")) {
                std::lock_guard<std::mutex> lock(server->mutex);
                server->requests.push_back(request);
                Response r = reply(429, "{\"error\":\"rate_limited\"}");
                r.retry_after = "600";
                return r;
            }
            return server->handle(request);
        }
    };
    auto held_server = std::make_shared<Server>();
    auto transport = std::make_unique<Hold>();
    transport->server = held_server;
    transport->refuse = &refuse;
    Client held(config, std::move(transport));
    held.track("tick");
    held.flush();
    REQUIRE(wait_for([&] { return held_server->count("/session") == 1; }));
    CHECK(held.identify("u1"));
    held.flush(); // an automatic attempt waits out the hold
    settle();
    CHECK_EQ(held_server->count("/session"), static_cast<std::size_t>(1));
    CHECK_EQ(held_server->count("/identify"), static_cast<std::size_t>(0));
}

RS_TEST(identify_is_sent_on_the_answered_token_whatever_the_device_clock_says) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<Server>();
    struct PastExpiry : Transport {
        std::shared_ptr<Server> server;
        Response send(const Request& request) override {
            Response response = server->handle(request);
            if (ends_with(request.url, "/session")) {
                // expiresAt already past by this device's clock.
                const std::string marker = "\"expiresIn\":3600";
                const std::size_t at = response.body.find(marker);
                if (at != std::string::npos) response.body.replace(at, marker.size(), "\"expiresAt\":1000");
            }
            return response;
        }
    };
    auto transport = std::make_unique<PastExpiry>();
    transport->server = server;
    Client client(identify_config(dir), std::move(transport));
    CHECK(client.identify("u1"));
    client.track("tick");
    client.flush();
    REQUIRE(wait_for([&] { return !server->sends().empty(); }));
    CHECK_EQ(server->sends()[0].first.rfind("S", 0), static_cast<std::size_t>(0));
}

RS_TEST(identify_a_playtest_run_keeps_it_in_memory_and_sends_nothing) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<Server>();
    Config config = identify_config(dir);
    config.playtest_token = "pt_token";
    config.playtest_run_id = "run1";
    Client client(config, std::make_unique<Wire>(server));
    CHECK(client.identify("u1"));
    open_session(client);
    settle();
    CHECK(client.is_identified());
    CHECK(server->sends().empty());
    CHECK(stored(dir).is_null());
}

RS_TEST(identify_failures_leave_the_last_error_alone) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<Server>();
    server->on_identify = [](const Request&) { return reply(503, "{\"error\":\"unavailable\"}"); };
    Client client(identify_config(dir), std::make_unique<Wire>(server));
    open_session(client);
    const std::string before = client.stats().last_error;
    CHECK(client.identify("u1"));
    REQUIRE(wait_for([&] { return server->sends().size() == 1; }));
    settle();
    CHECK_EQ(client.stats().last_error, before);
    CHECK(client.status() == Status::ok);
}

RS_TEST(identify_opt_out_and_reset_identity_delete_a_saved_identity_before_consent) {
    const std::string dir = rstest::make_temp_dir();
    auto server = std::make_shared<Server>();
    Config config = identify_config(dir);
    {
        Client client(config, std::make_unique<Wire>(server));
        CHECK(client.identify("u1"));
        open_session(client);
        REQUIRE(wait_for([&] { return server->sends().size() == 1; }));
    }
    // A later build asks for consent: the identity on disk is not kept before the yes.
    config.consent = Consent::opt_in;
    Client client(config, std::make_unique<Wire>(server));
    client.set_tracking_enabled(false);
    CHECK(stored(dir).is_null());
    client.reset_identity();
    CHECK(stored(dir).is_null());
    settle();
    CHECK_EQ(server->sends().size(), static_cast<std::size_t>(1));
}
