#pragma once
// Roblox links (roblox.com/games/..., roblox://, roblox-player:) opened in
// tonic. Only the place, a user to follow and a private server code are kept;
// the website's sign-in ticket is dropped unread.
#include <string>

namespace tonic {

struct JoinLink {
  long long place = 0;
  long long user = 0;     // join this user's server (follow)
  std::string link_code;  // private server
  bool any() const { return place > 0 || user > 0; }
};

bool link_parse(const std::string& text, JoinLink& out);
std::string link_describe(const JoinLink& l);  // "place 123 (private server)": safe to log

// The link tonic was started with (run.sh puts it in TONIC_LINK; also
// TONIC_JOIN after a restart). Taken once.
bool link_take_startup(JoinLink& out);

// Before restart_tonic: the restarted tonic joins L (TONIC_JOIN).
void link_for_restart(const JoinLink& l);

// Another tonic is already running (Main): hand it the link and return.
bool link_send(const JoinLink& l);
// The running tonic: a link handed over? (checks twice a second)
bool link_poll(JoinLink& out);

// Your desktop opens roblox:// and roblox-player: links with tonic.
bool links_handler_on();
// (They start tonic the way your app menu does: launcher_path().)
bool links_handler_set(bool on, std::string& error);
// On, but for another tonic (moved, or installed since): pointed at this one.
void links_handler_refresh();

}  // namespace tonic
