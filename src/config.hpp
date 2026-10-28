// config.hpp - tonic's own settings, NAME=VALUE lines in data/files/tonic.conf.
// Safe from any thread.
#pragma once

#include <string>

namespace tonic {

std::string config_get(const std::string& name, const std::string& fallback = "");
bool config_bool(const std::string& name, bool fallback);
int config_int(const std::string& name, int fallback);
void config_set(const std::string& name, const std::string& value);  // saved at once

}  // namespace tonic
