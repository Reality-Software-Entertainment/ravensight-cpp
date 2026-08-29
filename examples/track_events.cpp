// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Minimal tour of the SDK. Set RAVENSIGHT_INGEST_KEY to your gt_live_...
// key to run it against the live API; without one it exits immediately.

#include <cstdio>
#include <cstdlib>

#include <ravensight/ravensight.hpp>

int main() {
    const char* key = std::getenv("RAVENSIGHT_INGEST_KEY");
    if (key == nullptr || key[0] == '\0') {
        std::printf("Set RAVENSIGHT_INGEST_KEY to run this example.\n");
        return 0;
    }

    ravensight::Config config;
    config.ingest_key = key;
    config.game_version = "1.0.0";
    config.device_id_file = "ravensight_device_id.txt";
    config.log = [](const std::string& line) { std::printf("%s\n", line.c_str()); };

    ravensight::Client client(config);

    client.track("level_completed", {
        {"level", 3},
        {"deaths", 2},
        {"seconds", 41.7},
        {"boss", "cave troll"},
        {"hardcore", false},
    });

    client.submit_feedback("The boss fight drags", "balance", 3,
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
