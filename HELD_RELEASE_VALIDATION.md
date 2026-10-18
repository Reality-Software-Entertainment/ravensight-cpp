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
All 128 tests passed with 4,059 assertions, including strict document validation,
consent gating, ETag 304, disk-cache offline fallback and build isolation,
typed getter fallbacks, enqueue-time exposure labels and in-flight identity/opt-out discard.
The existing session-order fixtures now account for the automatic config request.
No live service requests, pushes, tags or publication were performed.
