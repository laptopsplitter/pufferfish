// profile.hpp - instances: more than one tonic at once, each signed in on its
// own (data/profiles/ID; Main uses data/). TONIC_PROFILE=ID picks one.
#pragma once

#include <string>
#include <vector>

namespace tonic {

struct Instance {
  std::string id;        // "" for Main
  std::string label;     // "Main", "Instance 2"
  std::string username;  // the Roblox account signed in there, when Roblox saved it
  bool signed_in = false;
  bool running = false;
  bool self = false;     // this tonic
};

// Before Roblox starts (--window runs): the instance's folders, the script's
// ${DATA}, its cookie file, and its lock. False: it is already running.
bool profile_setup(const std::string& here);

std::string profile_id();     // this instance ("" Main)
std::string profile_title();  // window title: "tonic", "tonic - Instance 2"
std::string profile_dir();    // its folder: data, or data/profiles/ID

std::vector<Instance> profile_list();
std::string profile_new();                     // a new, empty instance; its id
bool profile_launch(const std::string& id);    // starts it (its own window and launcher)
bool profile_delete(const std::string& id);    // not Main, not while it runs

}  // namespace tonic
