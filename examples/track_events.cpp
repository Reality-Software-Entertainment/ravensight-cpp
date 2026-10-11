// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Minimal tour of the SDK. Set RAVENSIGHT_INGEST_KEY to your gt_live_...
// key to run it against the live API; without one it exits immediately.

#include <cstdio>
#include <cstdlib>

#include <ravensight/ravensight.hpp>

int main(int argc, char** argv) {
    const char* key = std::getenv("RAVENSIGHT_INGEST_KEY");
    if (key == nullptr || key[0] == '\0') {
        std::printf("Set RAVENSIGHT_INGEST_KEY to run this example.\n");
        return 0;
    }

    ravensight::Config config;
    config.ingest_key = key;
    config.game_version = "1.0.0";
    config.log = [](const std::string& line) { std::printf("%s\n", line.c_str()); };
    config.on_status = [](ravensight::Status status) {
        std::printf("status: %s\n", status == ravensight::Status::ok ? "ok" : ravensight::to_string(status));
    };
    // During a Ravensight Playtest run the runner may pass these flags.
    ravensight::apply_playtest_args(config, argc, argv);

    ravensight::Client client(config);
    std::printf("device id: %s\n", client.device_id().c_str());

    client.track("level_completed", {
        {"level", 3},
        {"deaths", 2},
        {"seconds", 41.7},
        {"boss", "cave troll"},
        {"hardcore", false},
    });
    client.track("run_over", {{"level", "castle"}, {"rs_kind", "death"}});

    client.submit_feedback("The boss fight drags", "suggestion", 3,
                           [](const ravensight::FeedbackResult& result) {
                               std::printf("feedback: %s\n", result.ok ? "accepted" : result.code.c_str());
                           });

    if (client.flush_and_wait(10.0)) {
        std::printf("all events delivered\n");
    } else {
        std::printf("some events are still queued\n");
    }
    return 0;
}
