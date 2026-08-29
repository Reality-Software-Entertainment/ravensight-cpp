# Changelog

All notable changes to this library are documented here.

## [0.1.0] - 2026-08-28

First public beta.

### Added

* `ravensight::Client`, the public API: a background worker thread that
  reads the server kill switch at boot, opens sessions, runs a 5 second
  flush timer and performs every network request off the game thread.
* `ravensight::Queue`, the complete protocol state machine with no network,
  thread or clock dependency: batching at the 50 event server limit, the
  offline queue with a 500 event drop oldest cap, re-authentication on 401,
  `Retry-After` on 429, exponential backoff from 10 seconds to 5 minutes,
  and oversize batch splitting on 400.
* `ravensight::Transport` seam with a built in `CurlTransport` over libcurl.
* A small internal JSON writer and tolerant reader, so the SDK has no third
  party JSON dependency.
* Feedback submission and EXPERIMENTAL agent suggestions.
* `game_started` and `game_exited` lifecycle events, a local privacy toggle,
  and an optional persisted device id.
* Unit tests for the JSON layer, the state machine and the client (driven
  through a fake transport), plus a GitHub Actions workflow that builds and
  runs them on ubuntu-latest with gcc and clang.

### Known limitations

* The shutdown flush is best effort and bounded by the request timeout; a
  process that exits without destroying the `Client` loses whatever is still
  queued, since the queue lives in memory only.
* Not yet shipped inside a released game. See the beta notice in the README.
