// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// The pure pieces of automatic error capture (scrub, template, caps, trail,
// crash records) and, on POSIX, the real terminate and signal hooks in a
// forked child process. No network.

#include <csignal>
#include <cstdio>
#include <exception>
#include <thread>
#include <stdexcept>
#include <string>

#include "error_capture.hpp"
#include "ravensight/json.hpp"
#include "storage.hpp"
#include "temp_dirs.hpp"
#include "test_runner.hpp"

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace ravensight::detail;

RS_TEST(scrub_matches_the_contract_rules) {
    const std::pair<std::string, std::string> cases[] = {
        {"mail bob.smith+x@example.co.uk now", "mail [email] now"},
        {"GET https://api.example.com/v1/a?token=abc&x=1#frag failed",
         "GET https://api.example.com/v1/a failed"},
        {"at https://user:secret@host.example/path", "at https://host.example/path"},
        {"Authorization: Bearer abc.def-123_456", "Authorization: [token]"},
        {"jwt eyJhbGciOi.eyJzdWIiOiIx.sig-_x end", "jwt [token] end"},
        {"key gt_live_ab12CD34 and gt_mcp_x", "key [token] and [token]"},
        {"at /Users/jacob/game/main.cpp:1", "at ~/game/main.cpp:1"},
        {"at /home/al/game/main.cpp", "at ~/game/main.cpp"},
        {"at C:\\Users\\Jo Ann\\x.cpp", "at ~ Ann\\x.cpp"},
        {"at C:/Users/jo/x.cpp", "at ~/x.cpp"},
        {"from 192.168.1.10 ok", "from [ip] ok"},
        {"from 2001:db8::1 and ::1 ok", "from [ip] and [ip] ok"},
        {"full fe80:0:0:0:0:0:0:1", "full [ip]"},
        {"hash 9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08", "hash [token]"},
        {"digits only 12345678901234567890123456789012", "digits only [token]"},
        {"b64 QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVo0NTY3ODk=", "b64 [token]"},
    };
    for (const auto& c : cases) {
        CHECK_EQ(scrub(c.first), c.second);
    }
}

RS_TEST(scrub_matches_every_shared_vector) {
    // tests/fixtures/scrub-vectors.json is copied from the platform repo's
    // docs/errors/scrub-vectors.json; the server and every SDK match it.
    std::string text;
    REQUIRE(read_text_file(std::string(RAVENSIGHT_TEST_FIXTURES) + "/scrub-vectors.json", text));
    const ravensight::json::Value vectors = ravensight::json::Value::parse(text);
    const ravensight::json::Value* cases = vectors.find("cases");
    REQUIRE(cases != nullptr && cases->size() > 0);
    for (std::size_t i = 0; i < cases->size(); ++i) {
        const auto& c = cases->at(i);
        const std::string out = scrub(c.get_string("in", ""));
        if (!CHECK_EQ(out, c.get_string("out", ""))) {
            std::printf("    case: %s\n", c.get_string("name", "").c_str());
        }
    }
}

RS_TEST(scrub_catches_secrets_with_slashes) {
    CHECK_EQ(scrub("aws wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY1 leaked"),
             std::string("aws [token] leaked"));
    CHECK_EQ(scrub("blob QmFzZTY0/ZW5jb2RlZA2ZGF0YQ8+cGFkZGluZ3Rlc3Q= end"),
             std::string("blob [token] end"));
}

RS_TEST(scrub_leaves_code_and_paths_alone) {
    const std::string untouched[] = {
        "OnPointerClickHandlerImplementationForTheMenu failed",
        "at on_pointer_click_handler_for_the_menu (menu.cpp:4)",
        // The digit rule needs a digit in the RUN, and '/' ends a run, so a
        // long asset path with digits in it survives.
        "a deep asset path assets/sprites/player_01/idle_animation/frame_02.png",
        "/opt/build/game_2/src/gameplay/level_loader_v2/streaming_chunk_09.cpp:120",
        "cpp Foo::Bar at 12:30:45",
        "ravensight::detail::scrub(std::string const&)",
        "abc::def failed",
    };
    for (const auto& text : untouched) {
        CHECK_EQ(scrub(text), text);
    }
    CHECK_EQ(scrub(""), std::string(""));
}

RS_TEST(scrub_is_idempotent) {
    const std::string once =
        scrub("bob@x.io at https://h.io/a?b=1 from 10.0.0.1 gt_pub_abc Bearer t /home/me/x");
    CHECK_EQ(scrub(once), once);
}

RS_TEST(message_template_replaces_variable_parts) {
    CHECK_EQ(message_template("player 42 hit 3.5 damage"), std::string("player <n> hit <n> damage"));
    CHECK_EQ(message_template("missing 'sword_1' in deadbeef12"),
             std::string("missing <str> in <hex>"));
    CHECK_EQ(message_template("id 123e4567-e89b-12d3-a456-426614174000"), std::string("id <uuid>"));
    CHECK_EQ(message_template("load https://x.io/a/1 failed"), std::string("load <url> failed"));
}

RS_TEST(cut_chars_counts_code_points) {
    CHECK_EQ(cut_chars("h\xC3\xA9llo", 2), std::string("h\xC3\xA9"));
    CHECK_EQ(cut_chars("abc", 10), std::string("abc"));
}

RS_TEST(limiter_caps_per_key_per_minute_and_per_run) {
    CaptureLimiter limiter;
    double t = 1000.0;
    // Three per key.
    CHECK(limiter.allow("a", t));
    CHECK(limiter.allow("a", t));
    CHECK(limiter.allow("a", t));
    CHECK(!limiter.allow("a", t));
    // Six a minute: three more spend the bucket.
    CHECK(limiter.allow("b", t));
    CHECK(limiter.allow("c", t));
    CHECK(limiter.allow("d", t));
    CHECK(!limiter.allow("e", t));
    // Ten seconds refill one token.
    CHECK(limiter.allow("e", t + 10.0));
    CHECK(!limiter.allow("f", t + 10.0));
    CHECK_EQ(limiter.captured(), 7);

    // Thirty a run, however slowly they come.
    CaptureLimiter slow;
    int allowed = 0;
    for (int i = 0; i < 100; ++i) {
        if (slow.allow("k" + std::to_string(i), 60.0 * i)) {
            ++allowed;
        }
    }
    CHECK_EQ(allowed, captures_per_run);

    // Ten trails.
    int trails = 0;
    while (slow.take_trail()) {
        ++trails;
    }
    CHECK_EQ(trails, trails_per_run);
    slow.reset();
    CHECK(slow.take_trail());
    CHECK_EQ(slow.captured(), 0);
}

RS_TEST(trail_keeps_twenty_scrubbed_lines_in_order) {
    Trail trail;
    for (int i = 0; i < 25; ++i) {
        trail.add(i * 10, "info", "line " + std::to_string(i));
    }
    CHECK_EQ(trail.size(), static_cast<std::size_t>(20));
    const std::string text = trail.text();
    CHECK_EQ(text.substr(0, text.find('\n')), std::string("+50 info line 5"));
    CHECK(text.find("+240 info line 24") != std::string::npos);

    Trail one;
    one.add(5, "error", "mail a@b.io\nnow");
    CHECK_EQ(one.text(), std::string("+5 error mail [email] now"));
    one.add(-3, "info", "   ");
    CHECK_EQ(one.size(), static_cast<std::size_t>(1));
}

RS_TEST(trail_lines_and_total_are_cut) {
    Trail trail;
    for (int i = 0; i < 20; ++i) {
        trail.add(1, "info", std::string(300, 'x'));
    }
    const std::string text = trail.text();
    CHECK(text.size() <= trail_chars);
    CHECK_EQ(text.substr(0, text.find('\n')).size(), trail_line_chars);
}

RS_TEST(crash_records_round_trip) {
    CrashRecord record;
    record.type = "std::runtime_error";
    record.message = "save \"slot\" failed";
    record.timestamp = 1700000000;
    CrashRecord back;
    REQUIRE(parse_crash(serialize_crash(record), back));
    CHECK_EQ(back.type, record.type);
    CHECK_EQ(back.message, record.message);
    CHECK_EQ(back.timestamp, record.timestamp);
    CHECK(!parse_crash("not json", back));

    CrashRecord sig;
    REQUIRE(parse_signal_record("signal 11 1700000001\n", sig));
    CHECK_EQ(sig.type, std::string("SIGSEGV"));
    CHECK_EQ(sig.message, std::string("Fatal signal SIGSEGV"));
    CHECK_EQ(sig.timestamp, 1700000001LL);
    CHECK(!parse_signal_record("garbage", sig));
}

RS_TEST(describe_current_exception_reads_type_and_what) {
    try {
        throw std::out_of_range("index 7 past the end");
    } catch (...) {
        const CrashRecord record = describe_current_exception();
        CHECK_EQ(record.type, std::string("std::out_of_range"));
        CHECK_EQ(record.message, std::string("index 7 past the end"));
    }
    try {
        throw 42;
    } catch (...) {
        const CrashRecord record = describe_current_exception();
        CHECK_EQ(record.type, std::string("unknown"));
        CHECK_EQ(record.message, std::string("uncaught exception of type unknown"));
    }
    const CrashRecord none = describe_current_exception();
    CHECK_EQ(none.type, std::string("std::terminate"));
}

#if !defined(_WIN32)

namespace {

void throw_uncaught() noexcept(false) {
    throw std::runtime_error("save failed for /Users/jacob/slot1.sav");
}

/// Forks, runs body in the child (which must die), returns the wait status.
template <typename Body>
int in_child(Body body) {
    std::fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
        body();
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return status;
}

} // namespace

RS_TEST(terminate_handler_records_the_exception_and_chains) {
    const std::string dir = rstest::make_temp_dir();
    const std::string path = join_path(dir, "crash");
    const std::string signal_path = join_path(dir, "crash_signal");
    const int status = in_child([&] {
        install_terminate_handler(path);
        install_signal_handlers(signal_path);
        std::thread([] { throw_uncaught(); }).join();
    });
    // The previous handler (the default, std::abort) still ran.
    CHECK(WIFSIGNALED(status));
    CHECK_EQ(WTERMSIG(status), SIGABRT);
    std::string text;
    REQUIRE(read_text_file(path, text));
    CrashRecord record;
    REQUIRE(parse_crash(text, record));
    CHECK_EQ(record.type, std::string("std::runtime_error"));
    CHECK_EQ(record.message, std::string("save failed for /Users/jacob/slot1.sav"));
    CHECK(record.timestamp > 0);
    // The SIGABRT that followed did not write a second record.
    CHECK(!read_text_file(signal_path, text));
}

RS_TEST(terminate_handler_installs_once_and_uninstalls) {
    const std::string dir = rstest::make_temp_dir();
    const std::terminate_handler before = std::get_terminate();
    CHECK(install_terminate_handler(join_path(dir, "crash")));
    CHECK(install_terminate_handler(join_path(dir, "crash")));
    CHECK(terminate_handler_installed());
    uninstall_terminate_handler();
    CHECK(!terminate_handler_installed());
    CHECK(std::get_terminate() == before);
}

RS_TEST(signal_handler_writes_a_record_then_dies_as_before) {
    const std::string dir = rstest::make_temp_dir();
    const std::string path = join_path(dir, "crash_signal");
    const int status = in_child([&] {
        install_signal_handlers(path);
        std::raise(SIGSEGV);
    });
    CHECK(WIFSIGNALED(status));
    CHECK_EQ(WTERMSIG(status), SIGSEGV);
    std::string text;
    REQUIRE(read_text_file(path, text));
    CrashRecord record;
    REQUIRE(parse_signal_record(text, record));
    CHECK_EQ(record.type, std::string("SIGSEGV"));
}

namespace {
void game_own_handler(int) { _exit(42); }
} // namespace

RS_TEST(signal_handler_chains_to_the_game_handler) {
    const std::string dir = rstest::make_temp_dir();
    const std::string path = join_path(dir, "crash_signal");
    const int status = in_child([&] {
        std::signal(SIGFPE, &game_own_handler);
        install_signal_handlers(path);
        std::raise(SIGFPE);
    });
    CHECK(WIFEXITED(status));
    CHECK_EQ(WEXITSTATUS(status), 42);
    std::string text;
    CHECK(read_text_file(path, text));
}

#endif
