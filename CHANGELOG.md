# Changelog

All notable changes to this library are documented here.

## [0.3.0] - 2026-10-15

### Added

* Typed event helpers `Client::track_purchase`, `track_resource`,
  `track_progression` and `track_error`. Each builds the reserved `rs_*`
  properties over `Client::track`, upper-cases the currency, cuts the error
  message to 200 characters and the stack to 2000, lets the reserved keys
  win over the caller's extra properties, and accepts an event name
  override. Nothing is validated or thrown; the server flags a bad event.
* Opt-in health sampling: `Config::performance` (default false) and
  `Client::sample_frame(double dt_seconds)`, called by the game once per
  frame. It samples once a second into a reservoir of at most 600, and
  `shutdown()` sends one `health` event with the median and 10th
  percentile frames per second, memory in megabytes and boot time in
  milliseconds where the platform exposes them (macOS and Linux). Nothing
  is sent when no sample was taken.
* The session request carries `"sdk": "cpp/<version>"`.

## [0.2.0] - 2026-10-10

Brings the C++ SDK to the shared Ravensight SDK contract. Pre-1.0, so this
minor version contains breaking changes.

### Breaking

* The device id is saved by default, in a per user directory named after
  the ingest key (`%APPDATA%\ravensight\<hash>`, `~/Library/Application
  Support/ravensight/<hash>`, or `$XDG_DATA_HOME` or `~/.local/share` then
  `ravensight/<hash>`). Before, without `device_id_file` every launch was a
  new player. Set `Config::storage_dir` where there is no `HOME` or
  `APPDATA`.
* `game_started` is queued when the client starts, or, for a client
  started disabled or opted out, when it is first enabled; never twice.
* A refused ingest key (`401`, or a `403` other than the special codes) now
  stops automatic sending for the run instead of retrying forever, and a
  settings `401` no longer assumes tracking is on.
* `Config::api_url` defaults to the bare host `https://api.ravensight.io`;
  a trailing `/api/v1` is stripped and appended again, so either form works.
* `Client::device_id()` returns `std::string` by value (it can change).
* Feedback with a category outside the six accepted ones, or a rating
  outside 1 to 5, is refused locally (`invalid_category`,
  `invalid_rating`) instead of being sent. Feedback while disabled reports
  the reason code (`disabled`, `tracking_disabled`, ...).
* `track()` sends a batch as soon as 50 events are queued, not only on the
  timer.

### Added

* `Client::set_tracking_enabled(bool)` and the free function
  `ravensight::set_tracking_enabled(config, bool)`: the player's opt-out,
  saved to `Config::state_file` at once and read at construction. A saved
  opt-out wins over `Config::enabled`.
* `Client::reset_device_id()`: a new saved id, the queue cleared and the
  session dropped.
* `Client::status()`, `ravensight::Status`, `ravensight::to_string` and
  `Config::on_status`, using the shared reason codes `disabled`,
  `invalid_key`, `tracking_disabled`, `offline`, `rate_limited`,
  `session_failed` and `playtest_job_closed`.
* The playtest path: `Config::playtest_token` and `playtest_run_id` (and
  job id and persona), the `RAVENSIGHT_PLAYTEST_*` environment variables,
  and `ravensight::apply_playtest_args` for the command line flags. A
  playtest run uses `pt-<run id>` in memory only, sends the bare token in
  `X-Ravensight-Playtest`, tags events as synthetic, and stops on
  `playtest_job_closed` or `invalid_playtest_token`. `Client::is_playtest()`.
* `Client::session_token()` and `Config::join_token`, sent once as
  `joinToken`, with a fallback to a plain session on `invalid_join_token`.
* `ravensight::feedback_categories`, `ravensight::generate_device_id`,
  `ravensight::default_storage_dir`, `Config::storage_dir`,
  `Config::state_file`, `Stats::last_error`, and `version.hpp`.
* Tests for every item above, driven through a fake transport.

### Fixed

* A client built with `enabled = false` sent `GET /settings` at start, and
  `fetch_suggestions` sent its request while the client was opted out or
  kill switched. Neither touches the network now.
* `403 tracking_disabled` from `POST /session` (or any endpoint) now stops
  the run like the settings kill switch, instead of retrying.
* Device ids take 16 bytes from the operating system's random source
  (`BCryptGenRandom`, `getentropy` or `/dev/urandom`) instead of one 32 bit
  seed.
* A `202` arriving after the queue was cleared and refilled no longer
  erases the new events: batches are acknowledged by sequence number.
* `503` honors `Retry-After`. A batch `401` re-opens the session at most
  twice in a row, then backs off.
* The final flush at shutdown holds the quit for at most about 1.5 seconds
  (the built in transport aborts a request in flight at that deadline).
* The README and example sent the invalid feedback category `balance`;
  they use `suggestion`.

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
