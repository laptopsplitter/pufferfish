#include "account.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include "cookies.hpp"
#include "profile.hpp"
#include "spawn.hpp"

namespace tonic {

namespace {

#define LOG(fmt, ...) fprintf(stderr, "[account] " fmt, ##__VA_ARGS__)

std::mutex g_mu;
std::string g_status;
bool g_busy = false;

void set_status(const std::string& s) {
  std::lock_guard<std::mutex> l(g_mu);
  g_status = s;
}

// For a curl config file: "..." with \ and " escaped.
std::string quoted(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '\\' || c == '"') o += '\\';
    if (c != '\n' && c != '\r') o += c;
  }
  return o + "\"";
}

// The session and headers go to curl in a config file only you can read
// (-K), never on its command line, where other programs could see them.
struct CurlConf {
  std::string path;
  bool ok = false;
  CurlConf(const std::string& session, const std::string& extra) {
    path = profile_dir() + "/files/.tonic-request.conf";
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return;
    const std::string s = "cookie = " + quoted(".ROBLOSECURITY=" + session) + "\n" + extra;
    ok = write(fd, s.data(), s.size()) == static_cast<ssize_t>(s.size());
    close(fd);
  }
  ~CurlConf() { unlink(path.c_str()); }
};

// curl as you: returns the response headers + body (-D -), "" on failure.
std::string request(const std::string& session, const std::string& extra, const std::vector<std::string>& args) {
  CurlConf conf(session, extra);
  if (!conf.ok) return {};
  std::vector<std::string> argv = {"curl", "-sS", "-m", "15", "--proto", "=https", "-D", "-", "-K", conf.path};
  argv.insert(argv.end(), args.begin(), args.end());
  return spawn_read(argv, 20);
}

int http_status(const std::string& r) {  // the last status line (after any 100 Continue)
  int st = 0;
  for (size_t p = r.find("HTTP/"); p != std::string::npos; p = r.find("HTTP/", p + 5)) {
    const size_t sp = r.find(' ', p);
    if (sp != std::string::npos && (p == 0 || r[p - 1] == '\n')) st = atoi(r.c_str() + sp + 1);
  }
  return st;
}

std::string header(const std::string& r, const std::string& name) {
  std::string lr = r;
  for (char& c : lr) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  const size_t p = lr.find("\n" + name + ":");
  if (p == std::string::npos) return {};
  size_t a = p + name.size() + 2;
  while (a < r.size() && r[a] == ' ') ++a;
  const size_t e = r.find_first_of("\r\n", a);
  return r.substr(a, e - a);
}

void set_theme(const std::string& theme) {
  const std::string session = cookies_session();
  if (session.empty()) return set_status("Sign in first");
  set_status("Saving to your account...");
  // Who you are.
  const std::string me = request(session, "", {"https://users.roblox.com/v1/users/authenticated"});
  const size_t idp = me.find("\"id\":");
  const long long id = idp == std::string::npos ? 0 : strtoll(me.c_str() + idp + 5, nullptr, 10);
  if (id <= 0) {
    LOG("could not tell who is signed in (HTTP %d)\n", http_status(me));
    return set_status("Could not reach Roblox (signed out?)");
  }
  const std::string url = "https://accountsettings.roblox.com/v1/themes/User/" + std::to_string(id);
  const std::string body = std::string("{\"themeType\":\"") + (theme == "light" ? "Light" : "Dark") + "\"}";
  const std::vector<std::string> patch = {"-X", "PATCH", "-H", "Content-Type: application/json", "--data", body, url};
  // Roblox asks for a token (x-csrf-token) on the first try, like on its website.
  std::string r = request(session, "", patch);
  if (http_status(r) == 403) {
    const std::string token = header(r, "x-csrf-token");
    if (!token.empty()) r = request(session, "header = " + quoted("x-csrf-token: " + token) + "\n", patch);
  }
  const int st = http_status(r);
  LOG("account theme %s: HTTP %d\n", theme.c_str(), st);
  if (st >= 200 && st < 300)
    set_status(std::string("Your account is now ") + (theme == "light" ? "light" : "dark") + ": Roblox uses it when you press Play");
  else
    set_status("Roblox did not take it (HTTP " + std::to_string(st) + ")");
}

}  // namespace

void account_set_theme(const std::string& theme) {
  {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_busy) return;
    g_busy = true;
  }
  std::thread([theme] {
    set_theme(theme);
    std::lock_guard<std::mutex> l(g_mu);
    g_busy = false;
  }).detach();
}

std::string account_status() {
  std::lock_guard<std::mutex> l(g_mu);
  return g_status;
}

}  // namespace tonic
