// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Internal helpers for Ravensight campaign links (Client::set_campaign_link
// and the rs_link a web build's page address carries): the id check, the
// query string reader and, on an Emscripten build, the page's address. Not
// part of the public API; the tests include it.

#pragma once

#include <string>

namespace ravensight {
namespace detail {

/// The campaign link id, lower cased, when `text` is exactly 24 hex
/// characters in any case; otherwise empty. Never trims.
std::string campaign_link_id(const std::string& text);

/// The first `rs_link` of a page's location.search, read the way
/// URLSearchParams.get reads it (one leading '?' dropped, '+' is a space,
/// percent escapes decoded), when it is a link id; otherwise empty. Anything
/// from a '#' on is not part of location.search and is ignored.
std::string campaign_link_from_search(const std::string& search);

/// The page's location.search on an Emscripten build, read on the browser's
/// main thread. Empty on every other build, and when the page has none.
std::string page_search();

/// The link id in the page's address (campaign_link_from_search over
/// page_search) the first time it is called in this process; empty on
/// every later call, so a second client on the same page sends none.
std::string take_page_campaign_link();

} // namespace detail
} // namespace ravensight
