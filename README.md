# Ravensight for C++

Official C++ SDK for [Ravensight](https://ravensight.io) player analytics.

Sessions, batched events, an offline queue, rate limit handling and a server
side kill switch, in one small C++17 library. The only dependency is libcurl
for transport; JSON is handled by the SDK's own tiny writer and reader.

* API base: `https://api.ravensight.io/api/v1`
* Docs: https://ravensight.io/docs/
* Privacy: a random per-install device id, no advertising identifier, a saved player opt-out (`client.set_tracking_enabled(false)`) and a reset (`client.reset_device_id()`), no tracking permission prompt needed. What your store label can say: https://ravensight.io/docs/#privacy-label
* C++17, CMake 3.16 or newer, libcurl
* Shipping a build anyone can download? Read [Public builds](#public-builds).

## Verification status

Version 0.3.0 was built and its unit tests run on macOS 27.0.1 with Apple
clang 21.0.0, CMake 4.3.4 and libcurl 8.7.1, using the commands in
[Running the tests](#running-the-tests): 106 tests, all passing, with no
network involved. The CI workflow builds and runs the same suite on Linux
with gcc and clang on pushes to main and on pull requests; it has not run
on 0.3.0 yet. Windows (including the `BCryptGenRandom` and `%APPDATA%`
paths), Android, iOS and consoles have not been built or run, and the SDK
has **not** yet shipped inside a released game.

Read the code before you ship it in a release build, and please open an
issue with anything you hit.

## Install

### CMake FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(ravensight
    GIT_REPOSITORY https://github.com/Reality-Software-Entertainment/ravensight-cpp.git
    GIT_TAG v0.3.0)
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

ravensight::Client client(config);            // starts the worker thread

client.track("level_completed", {
    {"level", 3},
    {"deaths", 2},
    {"seconds", 41.7},
});
```

That is the whole setup. The client reads the server kill switch, opens a
session and sends queued events from its own background thread. The device
id is saved in a per user directory (see [Device id and storage](#device-id-and-storage)),
so the same install counts as the same player across launches. Destroying
the client (or calling `shutdown()`) sends `game_exited` and a final best
effort flush.

## API

Everything game code needs is on `ravensight::Client`:

| Call | What it does |
| --- | --- |
| `client.track(name, data)` | Queues an event. `data` is optional. Returns false when the client is not sending, or when the event's data is over 8,192 bytes as JSON (dropped). Non blocking. |
| `client.flush()` | Asks the worker to flush now. Non blocking. |
| `client.flush_and_wait(timeout)` | Flushes and waits for the queue to drain. Blocking. |
| `client.submit_feedback(message, category, rating, callback)` | Posts player feedback. The callback gets a `FeedbackResult`. |
| `client.fetch_suggestions(callback)` | EXPERIMENTAL. AI generated design suggestions for your game. |
| `client.set_tracking_enabled(bool)` | The player's analytics choice, saved and remembered across launches. |
| `client.reset_device_id()` | A fresh random identity from now on. Returns the new id. |
| `client.set_enabled(bool)` | Runtime on and off for your own logic. Not saved. |
| `client.is_collecting()` | True only while the client is collecting: a key, enabled, the player's choice allows it, the kill switch on and the key accepted. |
| `client.status()` | Why the client is or is not sending (see [Status and reasons](#status-and-reasons)). |
| `client.session_token()` | The current session token, empty until a session opens. |
| `client.device_id()` | The id in use. |
| `client.is_ready()` | True once a session exists. |
| `client.tracking_enabled()` | False once sending stopped for the run (kill switch, a refused key or a closed playtest). |
| `client.is_playtest()` | True during a Ravensight Playtest run. |
| `client.stats()` | Queued, sent, dropped, batch and session counters, and the last error code. |
| `client.shutdown()` | `game_exited`, a final flush and worker join. The destructor calls it too. |

Free functions: `ravensight::set_tracking_enabled(config, bool)` saves the
player's choice before a client exists, `ravensight::apply_playtest_args(config, argc, argv)`
reads the playtest command line flags, `ravensight::default_storage_dir(key)`
returns the default storage path, and `ravensight::feedback_categories` lists
the six feedback categories.

Event payloads are flat key/value pairs. Values can be strings, integers,
floating point numbers, bools or nullptr, written as a brace initializer
list at the call site. They are serialized by the SDK's own JSON writer, so
there is no third party JSON dependency to vendor.

### Feedback

```cpp
client.submit_feedback("The boss fight drags", "suggestion", 3,
    [](const ravensight::FeedbackResult& result) {
        if (!result.ok) {
            printf("feedback rejected: %s\n", result.code.c_str());
        }
    });
```

The category is optional (pass `""`) or one of `bug`, `suggestion`,
`complaint`, `praise`, `playtest`, `other` (`ravensight::feedback_categories`).
The rating is `0` to leave it out, or `1` to `5`. Anything else is refused
on the spot with `invalid_category` or `invalid_rating` and no request is
made; so is an empty message (`empty_message`) and any feedback while the
client is not sending (the code is the current reason, for example
`disabled`).

## Levels and deaths

The dashboard's level funnel, per level deaths and journeys read your event
names: names containing `died`, `destroyed` or `killed` count as deaths,
`start`, `begin` or `enter` as level starts, and `complet`, `finish`, `win`
or `clear` as completions. To say it explicitly, set `rs_kind` in the event
data to `level_start`, `level_complete`, `death`, or `none` to opt an event
out. It is read before the name, and an unknown value falls back to the
name. Keys that start with `rs_` are reserved for Ravensight.

```cpp
client.track("boss_phase_begin", {{"level", "castle"}, {"rs_kind", "none"}});
client.track("run_over", {{"level", "castle"}, {"rs_kind", "death"}});
```

## Configuration

| Field | Default | Meaning |
| --- | --- | --- |
| `ingest_key` | empty | Your publishable `gt_live_...` key, or a public `gt_pub_...` key. Empty makes the client inert (see [Public builds](#public-builds)). |
| `optional` | off | Silences the one warning an inert client logs. |
| `consent` | `Consent::opt_out` | `Consent::opt_in` sends nothing, and makes no device id, until `set_tracking_enabled(true)`. |
| `distribution` | empty | A short label for this build, sent on the session. At most 40 characters, no control characters, otherwise not sent. |
| `api_url` | `https://api.ravensight.io` | Only change this if you self host. Trailing slashes and a trailing `/api/v1` are accepted; `/api/v1` is always appended. |
| `game_version` | `1.0.0` | Reported to the server. |
| `platform` | detected | windows, macos, linux, android, ios or unknown, from compile time detection. |
| `device_id` | empty | Your own stable per install id. Empty uses a random saved `dev_` id. |
| `storage_dir` | per user default | Where the device id and the player's opt-out are saved. |
| `device_id_file` | `storage_dir/device_id` | Overrides the device id file. |
| `state_file` | `storage_dir/state` | Overrides the opt-out file. Beside `device_id_file` (named `ravensight_state`) when only that is set. |
| `max_queue_size` | `500` | Offline queue cap. Oldest events are dropped first. |
| `flush_interval_seconds` | `5` | Worker flush timer. `0` or less sends only on demand, at 50 queued events and at shutdown. |
| `request_timeout_seconds` | `15` | Per request timeout. |
| `track_lifecycle_events` | on | Sends `game_started` and `game_exited`, once each. |
| `enabled` | on | Start with the client off. Never saved. |
| `join_token` | empty | A session token from the player's client, sent once as `joinToken` (see [Dedicated servers](#dedicated-servers)). |
| `playtest_token`, `playtest_run_id`, `playtest_job_id`, `playtest_persona` | empty | Playtest overrides. Normally read from the environment (see [Playtest](#playtest)). |
| `log` | none | Optional log sink, called from the worker thread. Without one, warnings go to stderr. |
| `on_status` | none | Optional, called on the worker thread when `status()` changes. |

## Automatic events

Exactly two events are sent without a `track` call, unless
`track_lifecycle_events` is off:

* `game_started`, queued once when the client is constructed, so it is the
  first event and carries the real start time. A client constructed
  disabled or opted out queues it when it is first enabled instead, still
  once per run. If the server's kill switch is on, it is discarded unsent.
* `game_exited`, queued once in `shutdown()` (and so in the destructor),
  followed by one best effort flush that holds the quit for at most about
  1.5 seconds with the built in transport.

Pause and resume are not sent. The C++ SDK has no engine or operating
system lifecycle hooks, so it does not see a mobile app going to the
background: on iOS or Android, call `client.flush()` from your own
background handler. The background session rules the mobile SDKs follow
are not implemented here.

## Status and reasons

`client.status()` returns a `ravensight::Status`, and `ravensight::to_string`
turns it into the reason code shared by every Ravensight SDK. Set
`config.on_status` to be told when it changes (on the worker thread).

| Status | Code | Meaning |
| --- | --- | --- |
| `ok` | empty | Sending, or about to. |
| `no_key` | `no_key` | Built with an empty `ingest_key`. Inert for the client's lifetime. |
| `disabled` | `disabled` | `enabled` is false, `set_enabled(false)`, the player opted out, or `Consent::opt_in` and no yes yet. Nothing touches the network. |
| `invalid_key` | `invalid_key` | The key was refused (`401`, or a `403` other than the codes below) by the settings check, the session, a batch, feedback or suggestions. Logged once as `Ravensight: ingest key refused (invalid_key), sending stopped for this run. Check your gt_live_ key.` The run ends: the queue is cleared and nothing is retried, not even by `flush()`. A `401` on a batch is an expired session instead: the client opens a new one first. |
| `tracking_disabled` | `tracking_disabled` | The server kill switch, from `GET /settings` or a `403 tracking_disabled`. The queue is cleared and nothing more is sent this run. |
| `offline` | `offline` | No connection, a timeout or a server error. Retried from 10 seconds, doubling to 5 minutes, each wait spread by plus or minus 20 percent. |
| `rate_limited` | `rate_limited` | `429`, or `503` with `Retry-After`. The SDK waits exactly `Retry-After` seconds. |
| `session_failed` | `session_failed` | The session could not be opened, or a playtest token was invalid or came without a run id (those two stop the run). |
| `playtest_job_closed` | `playtest_job_closed` | The playtest job finished. Nothing more is sent this run. |

## Public builds

For a game anyone can download, fork or build from source: a demo, a jam
entry, an open source project. Nothing here changes a game that sets a key
and none of these options.

* **No key, no analytics.** A `Client` built with an empty `ingest_key` no
  longer throws. It is inert: no worker thread, no transport (libcurl is
  never touched), no file written, no request, and every call is a quiet
  no-op. `status()` is `Status::no_key`, `is_enabled()` and
  `is_collecting()` are false, and one warning is logged
  (`config.optional = true` silences it). So a public repository can ship
  with no key in it, and a clone or a development build sends nothing.
* **Inject the key at build time, never commit it.** Use a public `gt_pub_`
  key for builds anyone can download, passed in by your release build, for
  example as a compile definition:

  ```cmake
  # CMakeLists.txt: empty unless the release build passes -DRAVENSIGHT_KEY=...
  set(RAVENSIGHT_KEY "" CACHE STRING "Ravensight public key, set only by release builds")
  target_compile_definitions(my_game PRIVATE RAVENSIGHT_KEY="${RAVENSIGHT_KEY}")
  ```

  ```cpp
  config.ingest_key = RAVENSIGHT_KEY;  // empty in every build but a release
  config.optional = true;               // no warning in development builds
  ```

* **`Consent::opt_in`** sends nothing until the player says yes: no device
  id is made, and not even the settings check is sent, before
  `client.set_tracking_enabled(true)`. The yes is saved in the state file
  (`opt_in=true`), and `set_tracking_enabled(false)` replaces it with the
  opt-out. `Consent::opt_out`, the default, keeps the 0.2.0 behaviour.
  Events tracked before the yes are dropped (`track()` returns false), not
  held for later; events tracked right after it are queued while the
  settings check runs. `device_id()` is empty before the first yes. Saying
  no again stops everything but keeps the device id; to forget it too,
  call `reset_device_id()`.
* **`distribution`** is a short label of your own, such as
  `"native-release"`, `"itch-demo"` or `"source-build"`, sent on the session
  so you can tell builds apart. It is not sent when empty.
* **`rs_event_id`** is added to every event's data: the first 16
  characters of the device id, the start time in hex, a random 8 hex
  character value made once per client from the operating system's random
  source, and a counter. It stays with the queued event, so an event sent
  again after a failure carries the same id and the server can recognise
  it. The queue is in memory only; it is not saved between launches.
* **Size cap.** An event whose data is over 8,192 bytes as JSON is dropped
  before it is queued: `track()` returns false, `stats().dropped` counts
  it, `stats().last_error` is `event_too_large`, and one warning is logged
  for the run.
* **A refused key ends the run.** See `invalid_key` in
  [Status and reasons](#status-and-reasons). A later
  `set_tracking_enabled(true)` saves the choice and restarts nothing.

## Compatibility

| ravensight-cpp | C++ | API | Notes |
| --- | --- | --- | --- |
| 0.2.0 | C++17, CMake 3.16, libcurl | `/api/v1` | The shared SDK contract. |
| 0.3.0 | C++17, CMake 3.16, libcurl | `/api/v1` | Public builds. `gt_pub_` keys, the `distribution` field and recognising a resent `rs_event_id` need the API's public key support; an API without it ignores `distribution` and stores `rs_event_id` as an ordinary property. |

## About your ingest key

The `gt_live_...` key is publishable. It is safe to ship inside a build: it
can open sessions, read the tracking kill switch and read this game's design
suggestions. It cannot read analytics, read feedback or touch your account.
Rotate it from the dashboard at any time.

## How delivery works

* **Batching.** `track` only queues. The worker sends on its 5 second
  timer, as soon as 50 events are queued, on `flush()` and at shutdown, up
  to 50 events per request (the server limit), until the queue drains.
* **Offline queue.** Events accumulate up to `max_queue_size`. Past that the
  oldest are dropped so the newest are always kept.
* **Kept on failure.** An event leaves the queue when the server accepts it,
  or when the server refuses it on its own with `400` (it is dropped,
  counted in `stats().dropped` and logged once, so it cannot block the rest).
  The queue lives in memory: whatever is still queued when the process
  exits, or when the shutdown flush runs out of time, is lost.
* **Session expiry.** A `401` on a batch opens a new session and re sends
  the same batch, at most twice in a row, then backs off.
* **Rate limits.** A `429` (or a `503` with `Retry-After`) waits exactly
  `Retry-After` seconds. With no such header the SDK backs off
  exponentially from 10 seconds to a 5 minute ceiling.
* **Oversize batches.** A `400` halves the batch and retries down to a
  single event.
* **Kill switch.** `GET /settings` is read once, when the client first may
  send. If the server has tracking off for your game, the queue is cleared
  and nothing is sent.

## Threading contract

* The `Client` owns one background worker thread. Every network request,
  including feedback and suggestions, happens on that thread.
* `track`, `flush`, `set_enabled`, the getters and `stats` are thread safe
  and never block on the network. They take a short internal mutex, so they
  are safe to call from your game loop. `set_tracking_enabled` and
  `reset_device_id` also write a small file on the calling thread.
* `flush_and_wait` and `shutdown` block the calling thread.
* Callbacks (`submit_feedback`, `fetch_suggestions`, `log`, `on_status`)
  run on the worker thread, except a local refusal, which calls back at
  once on the calling thread. Do your own marshalling if you need them on
  your main thread, keep them quick, and do not call `shutdown` from inside
  one. `flush_and_wait` called from a callback runs inline instead of
  deadlocking.
* The `Client` must outlive its callbacks; destroying it joins the worker,
  so no callback runs after the destructor returns.

## Architecture

```
include/ravensight/
  ravensight.hpp       umbrella header, include this one
  client.hpp           Client, Config, Field, Status: the public API
  queue.hpp            Queue: the protocol state machine, clock injected
  transport.hpp        Request, Response, the Transport seam
  curl_transport.hpp   the built in libcurl transport
  json.hpp             small JSON writer and reader
  version.hpp          RAVENSIGHT_VERSION
src/
  storage.hpp/.cpp     storage directory, files, SHA-256, random bytes
```

`Queue` makes every batching, backoff and retry decision as a pure function
of its state plus a caller supplied clock reading. It has no libcurl, thread
or timer dependency, which is what lets the unit tests drive every retry
path deterministically. `Client` wires it to a real clock, a worker thread
and a `Transport`; hand it a fake transport and you can test the whole
protocol without a network.

## Running the tests

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The suite (`tests/`) runs with a fake transport and temporary directories:
no network, and nothing written to your real storage directory. It covers a
disabled start making no request, the saved opt-out across two clients,
reset, refused keys, the kill switch, `Retry-After`, the 400 split, the 50
event batch and 500 event cap, `api_url` forms, feedback validation, the
playtest path, the session hand-off, acknowledgment by sequence and the
lifecycle once-guards, plus the public build rules: an inert client with no
key, opt-in before and after the yes, `distribution` on the session body,
the backoff jitter bounds, the `rs_event_id` shape, resend and relaunch,
and the 8,192 byte cap.

## Device id and storage

The SDK makes a random device id, `dev_` plus 32 hex characters from 16
bytes of the operating system's random source, and saves it so the same
install is the same player across launches. It is saved only once the
client is enabled and the player has not opted out. By default it lives in
a per user directory named after your ingest key:

| Platform | Default storage directory |
| --- | --- |
| Windows | `%APPDATA%\ravensight\<hash>` |
| macOS and iOS | `~/Library/Application Support/ravensight/<hash>` |
| Linux and others | `$XDG_DATA_HOME/ravensight/<hash>`, else `~/.local/share/ravensight/<hash>` |

`<hash>` is the first 12 hex characters of the SHA-256 of the ingest key.
Set `config.storage_dir` (or `device_id_file` and `state_file`) on
platforms with no `HOME` or `APPDATA`, such as Android or consoles. If no
directory can be created, the SDK logs one warning and keeps the id in
memory for the run, so each launch counts as a new player.

No hardware identifier, advertising id, IP based fingerprint or personal
data is collected by the SDK itself. Only the events you choose to send
leave the machine.

## Opting out and resetting

```cpp
client.set_tracking_enabled(false);  // saved at once; nothing more is sent
client.set_tracking_enabled(true);   // back on, also saved

std::string fresh = client.reset_device_id(); // new identity, queue cleared
```

`set_tracking_enabled(false)` writes the choice to `state_file` before it
returns, clears the queue and stops all sending. Every later `Client` built
with the same storage reads it at construction, and a saved opt-out wins
over `enabled = true`: such a client makes no request of any kind. To save
the choice before a client exists (a first run settings screen, say), call
the free function with the config you will build the client from:

```cpp
ravensight::set_tracking_enabled(config, false);
```

`reset_device_id()` makes and saves a new id, clears the queue and drops the
session, so nothing links the old id to the new one. If you supply your own
`config.device_id`, the new id lasts for the run only.

`set_enabled(false)` is a runtime switch for your own logic (a level
editor, a debug build). It also stops sending, but it is not saved.

A game that sends gameplay events and nothing else can declare Device ID and
Product Interaction as not linked to the player and not used for tracking,
and needs no tracking permission prompt. The row by row answers for the App
Store and Google Play forms are at https://ravensight.io/docs/#privacy-label.

## Telling players

You do not need a permission prompt, but saying what the game sends is good
manners. A paragraph your game can show, in a settings screen or on its
store page:

> This game sends anonymous gameplay events (what happens in the game, never
> who you are) to Ravensight so we can improve it. You can turn this off in
> Settings at any time.

And the toggle behind "turn this off":

```cpp
// an "Analytics" switch in your settings screen
void set_analytics(ravensight::Client& client, bool on) {
    client.set_tracking_enabled(on);   // remembered across launches
}

// a "Forget me" button: a fresh random identity from now on
void forget_me(ravensight::Client& client) {
    client.reset_device_id();
}
```

## Dedicated servers

A game server can share the player's session, so its events land in the
same session as the player's own instead of counting a second player. The
player's client reads its token and sends it to your server however your
game talks to it:

```cpp
// player's game client
std::string token = client.session_token(); // empty until a session opens
```

The server passes it as `join_token`; it is sent once, as `joinToken`, when
the server's client opens its session. The server answers with a session
that shares the player's analytics session and device id (the body's
`deviceId` is ignored for a join):

```cpp
// dedicated server, one client per player connection
ravensight::Config config;
config.ingest_key = "gt_live_your_key";
config.device_id = player_device_id;         // used only if a plain session is needed
config.join_token = token_from_player;
config.track_lifecycle_events = false;       // the player's client sends those
ravensight::Client server_client(config);
server_client.track("match_result", {{"won", true}});
```

If the server refuses the token (`401 invalid_join_token`), the client opens
a plain session instead and records `invalid_join_token` in
`stats().last_error`. A session opened later (after the joined one expires)
is a plain one too. The session id itself is never computed on the client.

## Playtest

During a Ravensight Playtest run the runner gives the build a token. The
client reads it, first found wins, from `config.playtest_token` and
`config.playtest_run_id`, then the environment variables
`RAVENSIGHT_PLAYTEST_TOKEN` and `RAVENSIGHT_PLAYTEST_RUN_ID` (plus
`RAVENSIGHT_PLAYTEST_JOB_ID` and `RAVENSIGHT_PLAYTEST_PERSONA`, used only as
event tags), then the command line, which only your `main` can see:

```cpp
int main(int argc, char** argv) {
    ravensight::Config config;
    config.ingest_key = "gt_live_your_key";
    // --ravensight-playtest-token=... and --ravensight-playtest-run-id=...
    ravensight::apply_playtest_args(config, argc, argv);
    ravensight::Client client(config);
    // ...
}
```

With a token the client:

* uses `pt-<run id>` as the device id, in memory only, and never reads or
  writes the saved id or opt-out;
* sends the token in `X-Ravensight-Playtest` on every request, and tags
  every event with `synthetic`, `pt_run`, `pt_job` and `persona`;
* ignores the player's saved opt-out, and treats `set_tracking_enabled`
  and `reset_device_id` as no-ops that log one line;
* stops for the run on `403 playtest_job_closed` (`playtest_job_closed`) or
  `403 invalid_playtest_token` (`session_failed`), clearing the queue.

A token without a run id never runs as a real player: the client stops for
the run with `session_failed` and logs once.

## License

MIT. Copyright 2026 Reality Software Entertainment.
