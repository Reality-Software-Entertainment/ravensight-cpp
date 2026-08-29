# Ravensight for C++

Official C++ SDK for [Ravensight](https://ravensight.io) player analytics.

Sessions, batched events, an offline queue, rate limit handling and a server
side kill switch, in one small C++17 library. The only dependency is libcurl
for transport; JSON is handled by the SDK's own tiny writer and reader.

* API base: `https://api.ravensight.io/api/v1`
* Docs: https://ravensight.io/docs/
* C++17, CMake 3.16 or newer, libcurl

## BETA notice

This library is a beta. The protocol logic is written and reviewed against
the live API contract, it compiles cleanly on macOS (Apple clang) and Linux
(gcc and clang, in CI), and the full unit test suite runs on every push. It
has **not** yet shipped inside a released game.

Read the code before you ship it in a release build, and please open an
issue with anything you hit. The JavaScript and Godot SDKs speak the same
protocol and are further along.

## Install

### CMake FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(ravensight
    GIT_REPOSITORY https://github.com/Reality-Software-Entertainment/ravensight-cpp.git
    GIT_TAG v0.1.0)
FetchContent_MakeAvailable(ravensight)

target_link_libraries(your_game PRIVATE ravensight::ravensight)
```

### As a subdirectory

```bash
git clone https://github.com/Reality-Software-Entertainment/ravensight-cpp.git third_party/ravensight
```

```cmake
add_subdirectory(third_party/ravensight)
target_link_libraries(your_game PRIVATE ravensight::ravensight)
```

### System install

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build
```

Then `find_package(ravensight REQUIRED)` and link `ravensight::ravensight`.

The library builds static by default; pass `-DBUILD_SHARED_LIBS=ON` for a
shared library. libcurl must be findable via `find_package(CURL)`; on Debian
or Ubuntu that is `apt install libcurl4-openssl-dev`, on macOS it ships with
the system.

## Quickstart

```cpp
#include <ravensight/ravensight.hpp>

ravensight::Config config;
config.ingest_key = "gt_live_your_key";       // from the Ravensight dashboard
config.game_version = "1.0.0";
config.device_id_file = "ravensight_device_id.txt"; // optional persistence

ravensight::Client client(config);            // starts the worker thread

client.track("level_completed", {
    {"level", 3},
    {"deaths", 2},
    {"seconds", 41.7},
});
```

That is the whole setup. The client reads the server kill switch, opens a
session and flushes queued events every 5 seconds from its own background
thread. Destroying the client (or calling `shutdown()`) sends a final
best effort flush.

## API

Everything game code needs is on `ravensight::Client`:

| Call | What it does |
| --- | --- |
| `client.track(name, data)` | Queues an event. `data` is optional. Returns false when tracking is off. Non blocking. |
| `client.flush()` | Asks the worker to flush now. Non blocking. |
| `client.flush_and_wait(timeout)` | Flushes and waits for the queue to drain. Blocking. |
| `client.submit_feedback(message, category, rating, callback)` | Posts player feedback. The callback gets a `FeedbackResult`. |
| `client.fetch_suggestions(callback)` | EXPERIMENTAL. AI generated design suggestions for your game. |
| `client.set_enabled(bool)` | Local opt in and opt out for a privacy toggle. |
| `client.is_ready()` | True once a session exists. |
| `client.tracking_enabled()` | False when the server kill switch turned tracking off. |
| `client.stats()` | Queued, sent, dropped, batch and session counters. |
| `client.shutdown()` | Final flush and worker join. The destructor calls it too. |

Event payloads are flat key/value pairs. Values can be strings, integers,
floating point numbers, bools or nullptr, written as a brace initializer
list at the call site. They are serialized by the SDK's own JSON writer, so
there is no third party JSON dependency to vendor.

Feedback, with an optional callback:

```cpp
client.submit_feedback("The boss fight drags", "balance", 3,
    [](const ravensight::FeedbackResult& result) {
        if (!result.ok) {
            printf("feedback rejected: %s\n", result.code.c_str());
        }
    });
```

## Configuration

| Field | Default | Meaning |
| --- | --- | --- |
| `ingest_key` | none | Your publishable `gt_live_...` key. Required. |
| `api_url` | `https://api.ravensight.io/api/v1` | Only change this if you self host. `/api/v1` is appended when omitted. |
| `game_version` | `1.0.0` | Reported to the server. |
| `platform` | detected | windows, macos, linux, android, ios or unknown, from compile time detection. |
| `device_id` | generated | Stable per install id. Provide your own or set `device_id_file`. |
| `device_id_file` | none | Path where a generated id is persisted across runs. |
| `max_queue_size` | `500` | Offline queue cap. Oldest events are dropped first. |
| `flush_interval_seconds` | `5` | Worker flush timer. `0` flushes only on demand. |
| `request_timeout_seconds` | `15` | Per request timeout. |
| `track_lifecycle_events` | on | Sends `game_started` at boot and `game_exited` on shutdown. |
| `enabled` | on | Start opted out by setting this false. |
| `log` | none | Optional log sink, called from the worker thread. |

## About your ingest key

The `gt_live_...` key is publishable. It is safe to ship inside a build: it
can only open sessions and read the tracking kill switch. It cannot read
analytics, read feedback or touch your account. Rotate it from the dashboard
at any time.

## How delivery works

* **Batching.** Up to 50 events per request, the server hard limit. A flush
  keeps sending batches until the queue drains.
* **Offline queue.** Events accumulate up to `max_queue_size`. Past that the
  oldest are dropped so the newest are always kept.
* **Never dropped on failure.** Events leave the queue only after the server
  answers `202`.
* **Session expiry.** A `401` clears the session, opens a new one and re
  sends the same batch.
* **Rate limits.** A `429` honors `Retry-After` in seconds. With no such
  header the SDK backs off exponentially from 10 seconds to a 5 minute
  ceiling. The queue is held, never discarded.
* **Oversize batches.** A `400` halves the batch and retries down to a
  single event. An event still rejected on its own is dropped so it cannot
  block everything behind it.
* **Kill switch.** `GET /settings` is read once at boot. If the server has
  tracking off for your game, the queue is cleared and nothing is sent.

Flushes happen on the 5 second timer, on explicit `flush()` and
`flush_and_wait()` calls, and once more during `shutdown()`. The shutdown
flush is best effort and bounded by the request timeout; anything not
delivered is gone, since the queue lives in memory only.

## Threading contract

* The `Client` owns one background worker thread. Every network request,
  including feedback and suggestions, happens on that thread.
* `track`, `flush`, `set_enabled`, the getters and `stats` are thread safe
  and never block on the network. They take a short internal mutex, so they
  are safe to call from your game loop.
* `flush_and_wait` and `shutdown` block the calling thread.
* Callbacks (`submit_feedback`, `fetch_suggestions`, `log`) run on the
  worker thread. Do your own marshalling if you need them on your main
  thread, keep them quick, and do not call `shutdown` from inside one.
  `flush_and_wait` called from a callback runs inline instead of
  deadlocking.
* The `Client` must outlive its callbacks; destroying it joins the worker,
  so no callback runs after the destructor returns.

## Architecture

```
include/ravensight/
  ravensight.hpp       umbrella header, include this one
  client.hpp           Client, Config, Field: the public API
  queue.hpp            Queue: the protocol state machine, clock injected
  transport.hpp        Request, Response, the Transport seam
  curl_transport.hpp   the built in libcurl transport
  json.hpp             small JSON writer and reader
```

`Queue` makes every batching, backoff and retry decision as a pure function
of its state plus a caller supplied clock reading. It has no libcurl, thread
or timer dependency, which is what lets the unit tests drive every retry
path deterministically. `Client` wires it to a real clock, a worker thread
and a `Transport`; hand it a fake transport and you can test the whole
protocol without a network.

Run the tests:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

## Device id and privacy

The SDK generates a random device id on first run. Set
`config.device_id_file` to persist it across runs, or supply your own stable
id in `config.device_id`; with neither, each run looks like a new device. No
hardware identifier, advertising id, IP based fingerprint or personal data
is collected by the SDK itself. Only the events you choose to send leave the
machine.

`client.set_enabled(false)` stops all sending and discards anything still
queued, so an opt out does not leave player data sitting in memory.

## License

MIT. Copyright 2026 Reality Software Entertainment.
