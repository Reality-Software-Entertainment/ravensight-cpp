// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Umbrella header. #include <ravensight/ravensight.hpp> and you have the
// whole SDK: Client for game code, plus Queue, Transport and the json
// helpers for tests and custom integrations.

#pragma once

#define RAVENSIGHT_VERSION_MAJOR 0
#define RAVENSIGHT_VERSION_MINOR 1
#define RAVENSIGHT_VERSION_PATCH 0
#define RAVENSIGHT_VERSION "0.1.0"

#include "ravensight/client.hpp"
#include "ravensight/curl_transport.hpp"
#include "ravensight/json.hpp"
#include "ravensight/queue.hpp"
#include "ravensight/transport.hpp"
