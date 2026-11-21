# Held combined release, October 17, 2026

Public-build support and typed events have been combined locally. This record
is verification evidence, not a publication or deployment record.

Validated: 124 tests with 4,027 assertions.

The merge preserves public-key consent and inert-without-key behavior,
stable event IDs, typed event helpers and opt-in performance sampling.
JavaScript now defers performance listeners until consent; Android health
summaries carry the same deduplication IDs as other events.

Physical-device and live-service validation is not implied by these checks.
Publishing, tags, marketplace submission and deployments remain held pending
owner approval. Older README test counts describe their earlier branch runs;
this file records the combined candidate.

## 0.3.1 remote configuration and experiments

Local CMake C++17 library and example build succeeded on October 17, 2026.
All 128 tests passed with 4,061 assertions, including strict document validation,
consent gating, ETag 304, disk-cache offline fallback and build isolation,
typed getter fallbacks, exact 64-bit event integer preservation, enqueue-time exposure labels and in-flight identity/opt-out discard.
The existing session-order fixtures now account for the automatic config request.
No live service requests, pushes, tags or publication were performed.

## Player Reports follow-up

- Local C++17 library/example/test build passes.
- Full native test binary: **134 cases, 4,103 assertions, zero failures**.
- New cases cover copied binary bytes, session-authenticated API requests, credential-free
  multipart, processing receipts, same-handle retry, reset/opt-out races, failed upload
  without fallback/finalization, invalid grants and bounded preparation.
- Evidence: `/tmp/rs-cpp-reports-build.log`, `/tmp/rs-cpp-reports-tests.log`.
- Tests use scripted transport. Actual storage uploads, packaged games and device integration remain
  release checks; no publishing, tagging, pushes or deployments were performed.

## 0.6.0: identify players who sign in (November 19, 2026)

- Branch `feat/identify`, built to the platform's identify contract version 2.
- `cmake` Release build and `ravensight_tests`: **239 tests passed**, 0 failed
  (213 before), every identify case against a fake transport.
- No live Ravensight service was used. Nothing was published, tagged or deployed.

### Campaign links (same 0.6.0)

- Branch `feat/campaign-link`, on `feat/identify` at 2c6d47a, built to the
  campaign link contract (the JavaScript SDK is the reference).
- `cmake` Release build and `ravensight_tests`: **261 tests passed**, 5,057
  assertions, 0 failures, no build warnings; the red run failed in the new
  cases only. The cases cover the setter, 25 `rs_link` parsing vectors, once
  per session, kept after a 500, no answer, a 429 and a 201 without a token,
  a link set while a session is in flight, opt in, opt out, disabled, no key,
  a playtest run and a join token. The pre-push `ctest` passed again on the
  final head.
- The web is not a supported target. The compile-guarded Emscripten page read
  was built with an Emscripten 4.0.20 toolchain and run under node with a
  stubbed page (18 of 18), not in a browser.
- The README deep link example was compiled and run (7 of 7).
- No live Ravensight service was used. Nothing was published, tagged or deployed.

### Release candidate (November 21, 2026)

- `feat/campaign-link` and `feat/identify` merged onto `main` at babcb32 as
  0.6.0. `cmake` Release build and `ctest` on macOS (Apple clang): **261 tests
  passed**, 5,057 assertions, 0 failures. Ubuntu gcc and clang run in CI;
  Windows was not built here.
