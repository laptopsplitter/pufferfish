// make test-links: Roblox links to (place, user, private server) and nothing else.
#include <cstdio>
#include <string>

#include "../src/links.hpp"

using tonic::JoinLink;

int fails = 0;
void expect(const char* url, long long place, long long user, const char* code) {
  JoinLink l;
  const bool ok = tonic::link_parse(url, l);
  const bool want = place > 0 || user > 0;
  if (ok != want || l.place != place || l.user != user || l.link_code != code) {
    printf("FAIL %s -> %d place %lld user %lld code '%s'\n", url, ok, l.place, l.user, l.link_code.c_str());
    ++fails;
  }
}

int main() {
  expect("https://www.roblox.com/games/920587237/Adopt-Me", 920587237, 0, "");
  expect("  https://web.roblox.com/games/606849621/Jailbreak?privateServerLinkCode=abc123-XY_9  ", 606849621, 0, "abc123-XY_9");
  expect("https://www.roblox.com/games/start?placeId=1818&launchData=x", 1818, 0, "");
  expect("920587237", 920587237, 0, "");
  expect("roblox://placeId=1818", 1818, 0, "");
  expect("roblox://experiences/start?placeId=1818&linkCode=Q1", 1818, 0, "Q1");
  expect("roblox-player:1+launchmode:play+gameinfo:SECRETTICKET+launchtime:1+placelauncherurl:"
         "https%3A%2F%2Fassetgame.roblox.com%2Fgame%2FPlaceLauncher.ashx%3Frequest%3DRequestGame%26browserTrackerId%3D1"
         "%26placeId%3D1818%26isPlayTogetherGame%3Dfalse+browsertrackerid:1+robloxLocale:en_us",
         1818, 0, "");
  expect("roblox-player:1+launchmode:play+gameinfo:T+placelauncherurl:https%3A%2F%2Fassetgame.roblox.com%2Fgame%2F"
         "PlaceLauncher.ashx%3Frequest%3DRequestFollowUser%26userId%3D42+x:y",
         0, 42, "");
  expect("roblox-player:1+launchmode:play+gameinfo:T+placelauncherurl:https%3A%2F%2Fevil.example%2F%3FplaceId%3D5", 0, 0, "");
  expect("https://evil.example/games/123", 0, 0, "");
  expect("https://roblox.com.evil.example/games/123", 0, 0, "");
  expect("https://www.roblox.com/games/1/x?privateServerLinkCode=bad\"code", 1, 0, "");
  expect("hello", 0, 0, "");
  expect("", 0, 0, "");
  // The ticket is never part of what is kept.
  JoinLink l;
  tonic::link_parse("roblox-player:1+gameinfo:SECRETTICKET+placelauncherurl:https%3A%2F%2Fassetgame.roblox.com%2F%3FplaceId%3D7", l);
  if (tonic::link_describe(l).find("SECRET") != std::string::npos || l.link_code.find("SECRET") != std::string::npos) {
    printf("FAIL: the ticket was kept\n");
    ++fails;
  }
  printf(fails ? "links: %d failed\n" : "links: ok\n", fails);
  return fails != 0;
}
