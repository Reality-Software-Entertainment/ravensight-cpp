#pragma once
#include "ravensight/json.hpp"
namespace ravensight { namespace detail {
struct ConfigSnapshot { json::Value values, experiments; std::string etag; };
bool parse_config(const std::string& body, ConfigSnapshot& out);
bool read_config_cache(const std::string& path, ConfigSnapshot& out);
std::string url_component(const std::string& value);
} }
