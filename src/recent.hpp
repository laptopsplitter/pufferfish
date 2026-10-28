// recent.hpp - games you played, newest first, for quick join from the menu:
// data/files/tonic-recent.txt (PLACE_ID \t UNIX_TIME \t NAME). Names come
// from Roblox's public games API (curl), looked up in the background.
#pragma once

#include <string>
#include <vector>

namespace tonic {

struct RecentGame {
  long long place = 0;
  long long when = 0;
  std::string name;  // empty until looked up
};

void recent_add(long long place);       // a game was launched
std::vector<RecentGame> recent_list();  // newest first, at most 8
void recent_fetch_names();              // looks up missing names (background)
// The signed-in account (username; "" logged out): switching clears the list,
// so each account only sees the games it played.
void recent_set_owner(const std::string& account);

}  // namespace tonic
