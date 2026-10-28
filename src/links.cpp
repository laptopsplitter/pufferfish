#include "links.hpp"
#include "platform.hpp"
#include "appicon.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "spawn.hpp"

namespace tonic {

namespace {

#define LOG(fmt, ...) fprintf(stderr, "[links] " fmt, ##__VA_ARGS__)
namespace fs = std::filesystem;

constexpr const char* kRequest = "data/join-request";  // Main's: links go to Main
constexpr const char* kDesktop = "tonic-links.desktop";
constexpr const char* kSchemes[] = {"x-scheme-handler/roblox", "x-scheme-handler/roblox-player"};

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string url_decode(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      o += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else {
      o += s[i];
    }
  }
  return o;
}

// The value of KEY=VALUE in S (KEY not part of a longer name; any case).
std::string param(const std::string& s, const std::string& key) {
  const std::string ls = lower(s), lk = lower(key) + "=";
  for (size_t p = ls.find(lk); p != std::string::npos; p = ls.find(lk, p + 1)) {
    if (p > 0 && (std::isalnum(static_cast<unsigned char>(ls[p - 1])) || ls[p - 1] == '_')) continue;
    size_t e = p + lk.size();
    std::string v;
    while (e < s.size() && s[e] != '&' && s[e] != '+' && s[e] != '#' && s[e] != ' ') v += s[e++];
    return v;
  }
  return {};
}

long long number(const std::string& s) {
  if (s.empty() || s.size() > 18 || !std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; })) return 0;
  return std::stoll(s);
}

bool code_ok(const std::string& c) {  // link codes: letters, digits, - and _
  return !c.empty() && c.size() <= 64 &&
         std::all_of(c.begin(), c.end(), [](char x) { return std::isalnum(static_cast<unsigned char>(x)) || x == '-' || x == '_'; });
}

std::string data_home() {
  if (const char* x = getenv("XDG_DATA_HOME"); x && *x == '/') return x;
  const char* h = getenv("HOME");
  return std::string(h ? h : ".") + "/.local/share";
}
std::string config_home() {
  if (const char* x = getenv("XDG_CONFIG_HOME"); x && *x == '/') return x;
  const char* h = getenv("HOME");
  return std::string(h ? h : ".") + "/.config";
}

// For a .desktop Exec line: in double quotes, with " ` $ \ escaped.
std::string desktop_quote(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '`' || c == '$' || c == '\\') o += '\\';
    o += c;
  }
  return o + "\"";
}

std::string encode(const JoinLink& l) {
  return std::to_string(l.place) + "\t" + std::to_string(l.user) + "\t" + l.link_code;
}
bool decode(const std::string& s, JoinLink& out) {
  std::stringstream ss(s);
  std::string a, b, c;
  std::getline(ss, a, '\t');
  std::getline(ss, b, '\t');
  std::getline(ss, c, '\n');
  out = {};
  out.place = number(a);
  out.user = number(b);
  if (code_ok(c)) out.link_code = c;
  return out.any();
}

}  // namespace

bool link_parse(const std::string& text, JoinLink& out) {
  out = {};
  std::string s = text;
  s.erase(0, s.find_first_not_of(" \t\r\n\"'<"));
  while (!s.empty() && std::strchr(" \t\r\n\"'>", s.back())) s.pop_back();
  if (s.empty() || s.size() > 8192) return false;
  if (const long long n = number(s)) {  // just a place id
    out.place = n;
    return true;
  }
  const std::string ls = lower(s);
  std::string q;  // where the parameters are
  if (ls.rfind("roblox-player:", 0) == 0) {
    // The website's Play: + separated KEY:VALUE; the game is in placelauncherurl.
    // (gameinfo:, the sign-in ticket, is never looked at.)
    const size_t p = ls.find("+placelauncherurl:");
    if (p == std::string::npos) return false;
    size_t e = s.find('+', p + 1);
    q = url_decode(s.substr(p + 18, e == std::string::npos ? std::string::npos : e - p - 18));
    if (lower(q).find("roblox.com/") == std::string::npos) return false;  // not a Roblox place launcher
  } else if (ls.rfind("roblox://", 0) == 0 || ls.rfind("roblox:", 0) == 0) {
    q = url_decode(s);
  } else {
    // A web link: www.roblox.com/games/ID/..., roblox.com/games/start?placeId=ID
    std::string host = ls;
    for (const char* pre : {"https://", "http://"})
      if (host.rfind(pre, 0) == 0) host = host.substr(strlen(pre));
    const std::string h = host.substr(0, host.find('/'));
    if (h != "roblox.com" && (h.size() < 11 || h.compare(h.size() - 11, 11, ".roblox.com") != 0)) return false;
    q = url_decode(s);
    const size_t g = lower(q).find("/games/");
    if (g != std::string::npos) {
      std::string id;
      for (size_t i = g + 7; i < q.size() && std::isdigit(static_cast<unsigned char>(q[i])); ++i) id += q[i];
      out.place = number(id);
    }
  }
  if (!out.place) out.place = number(param(q, "placeId"));
  out.user = number(param(q, "userId"));
  if (lower(q).find("followuser") == std::string::npos && lower(q).find("userid") != std::string::npos && out.place > 0)
    out.user = 0;  // a userId next to a place only says who shared it
  for (const char* k : {"privateServerLinkCode", "linkCode"}) {
    const std::string c = param(q, k);
    if (code_ok(c)) {
      out.link_code = c;
      break;
    }
  }
  return out.any();
}

std::string link_describe(const JoinLink& l) {
  std::string s = l.place > 0 ? "place " + std::to_string(l.place) : "a friend's game";
  if (l.user > 0) s += " (following user " + std::to_string(l.user) + ")";
  if (!l.link_code.empty()) s += " (private server)";
  return s;
}

bool link_take_startup(JoinLink& out) {
  for (const char* var : {"TONIC_JOIN", "PUFFERFISH_LINK"}) {
    const char* v = getenv(var);
    if (!v || !*v) continue;
    const std::string text = v;
    unsetenv(var);  // once; and a restart does not join again
    const bool ok = std::string(var) == "TONIC_JOIN" ? decode(text, out) : link_parse(text, out);
    if (ok) {
      LOG("opened with a link: %s\n", link_describe(out).c_str());
      return true;
    }
    LOG("opened with a link that is not a Roblox game link; ignored\n");
  }
  return false;
}

void link_for_restart(const JoinLink& l) { setenv("TONIC_JOIN", encode(l).c_str(), 1); }

bool link_send(const JoinLink& l) {
  const std::string tmp = std::string(kRequest) + ".new";
  const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  const std::string s = encode(l) + "\n";
  const bool ok = write(fd, s.data(), s.size()) == static_cast<ssize_t>(s.size());
  close(fd);
  return ok && rename(tmp.c_str(), kRequest) == 0;
}

bool link_poll(JoinLink& out) {
  static auto last = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::milliseconds(500)) return false;
  last = now;
  struct stat st{};
  if (stat(kRequest, &st) != 0) return false;
  std::ifstream f(kRequest);
  std::string line;
  std::getline(f, line);
  f.close();
  unlink(kRequest);
  if (st.st_uid != getuid() || !decode(line, out)) return false;
  LOG("link from the browser: %s\n", link_describe(out).c_str());
  return true;
}

bool links_handler_on() {
  std::error_code ec;
  if (!fs::exists(data_home() + "/applications/" + kDesktop, ec)) return false;
  std::ifstream f(config_home() + "/mimeapps.list");
  std::string line;
  while (std::getline(f, line))
    if (line.rfind("x-scheme-handler/roblox-player=", 0) == 0) return line.find(kDesktop) != std::string::npos;
  return false;
}

void links_handler_refresh() {
  const std::string file = data_home() + "/applications/" + kDesktop;
  std::ifstream f(file);
  if (!f) return;
  std::string line, err;
  while (std::getline(f, line))
    if (line.rfind("Exec=", 0) == 0) {
      if (line == "Exec=" + desktop_quote(launcher_path()) + " --link %u") return;
      f.close();
      LOG("Roblox links pointed at another tonic: now at %s\n", launcher_path().c_str());
      links_handler_set(true, err);
      return;
    }
}

bool links_handler_set(bool on, std::string& error) {
  const std::string apps = data_home() + "/applications", file = apps + "/" + kDesktop;
  std::error_code ec;
  if (on) {
    fs::create_directories(apps, ec);
    std::ofstream f(file, std::ios::trunc);
    f << "[Desktop Entry]\n"
         "Type=Application\n"
         "Name=tonic\n"
         "Comment=Opens Roblox links (Play on roblox.com) in tonic\n"
         "Exec="
      << desktop_quote(launcher_path()) << " --link %u\n"
      << "Terminal=false\n";
    if (const std::string icon = app_icon_path(); !icon.empty()) f << "Icon=" << icon << "\n";
    f << "NoDisplay=true\n"
         "MimeType=x-scheme-handler/roblox;x-scheme-handler/roblox-player;\n";
    f.close();
    if (!f) {
      error = "could not write " + file;
      return false;
    }
    int st = -1;
    spawn_read({"xdg-mime", "default", kDesktop, kSchemes[0], kSchemes[1]}, 10, &st);
    spawn_read({"update-desktop-database", apps}, 10);
    if (st != 0 || !links_handler_on()) {
      error = st == -1 ? "xdg-mime is missing (Arch: xdg-utils)" : "your desktop did not take it (xdg-mime)";
      return false;
    }
    LOG("Roblox links now open in tonic\n");
    return true;
  }
  // Off: our lines out of mimeapps.list, our .desktop file away.
  const std::string list = config_home() + "/mimeapps.list";
  std::ifstream in(list);
  if (!in) {
    fs::remove(file, ec);
    return true;
  }
  std::stringstream keep;
  std::string line;
  while (std::getline(in, line)) {
    const bool ours = line.rfind("x-scheme-handler/roblox", 0) == 0 && line.find(kDesktop) != std::string::npos;
    if (!ours) keep << line << "\n";
  }
  in.close();
  {
    std::ofstream o(list + ".tonic-new", std::ios::trunc);
    o << keep.str();
  }
  fs::rename(list + ".tonic-new", list, ec);
  fs::remove(file, ec);
  spawn_read({"update-desktop-database", apps}, 10);
  LOG("Roblox links no longer open in tonic\n");
  return true;
}

}  // namespace tonic
