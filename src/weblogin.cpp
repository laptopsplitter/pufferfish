#include "platform.hpp"
#include "weblogin.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cookies.hpp"
#include "fakejni.hpp"
#include "profile.hpp"
#include "recent.hpp"

extern char** environ;

namespace tonic {

namespace {

#define LOG(fmt, ...) fprintf(stderr, "[signin] " fmt, ##__VA_ARGS__)

std::atomic<bool> g_running{false};
std::mutex g_mu;
std::string g_status;

std::string account_path() { return profile_dir() + "/files/tonic-account.txt"; }

void set_status(const std::string& s) {
  std::lock_guard<std::mutex> l(g_mu);
  g_status = s;
}

std::string helper_path() {
  const std::string d = self_exe();
  if (d.empty()) return "pufferfish-webview";
  return d.substr(0, d.rfind('/') + 1) + "pufferfish-webview";
}

}  // namespace

bool weblogin_running() { return g_running; }

void weblogin_forget() {
  set_status("");
  unlink(account_path().c_str());
}

// JSON string escaping for the few values written below.
std::string json_quote(const std::string& v) {
  std::string o = "\"";
  for (char c : v) {
    if (c == '"' || c == '\\') o += '\\', o += c;
    else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
    else o += c;
  }
  return o + "\"";
}

// Roblox's app keeps who is signed in in its own storage
// (files/appData/LocalStorage/appStorage.json: UserId, Username, DisplayName)
// and starts at the welcome screen when it names no one, whatever the session.
// A sign-in on Roblox's own screen fills these in; the website sign-in does
// here, before Roblox starts. Values keep the type they already have there.
std::filesystem::path app_storage_path() { return profile_dir() + "/files/appData/LocalStorage/appStorage.json"; }

std::string app_storage_read() {
  std::ifstream in(app_storage_path());
  std::stringstream ss;
  ss << in.rdbuf();
  std::string json = ss.str();
  if (json.find('{') == std::string::npos) json = "{}";
  return json;
}

void app_storage_write(const std::string& json) {
  const std::filesystem::path f = app_storage_path(), tmp = f.string() + ".tonic-new";
  std::error_code ec;
  std::filesystem::create_directories(f.parent_path(), ec);
  {
    std::ofstream out(tmp, std::ios::trunc);
    out << json;
  }
  std::filesystem::rename(tmp, f, ec);
}

// "KEY": VALUE in Roblox's flat appStorage.json (VALUE a JSON string or a bare word/number).
const std::regex& key_re(const std::string& key, std::regex& keep) {
  keep = std::regex("\"" + key + "\"\\s*:\\s*(\"(?:[^\"\\\\]|\\\\.)*\"|[^,}\\s]*)");
  return keep;
}

// Sets KEY, keeping a value's type (a number stays a number when AS_NUMBER is given).
void put_key(std::string& json, const std::string& key, const std::string& as_string, const std::string& as_number) {
  std::regex re;
  std::smatch m;
  if (std::regex_search(json, m, key_re(key, re))) {
    const bool was_string = m[1].length() > 0 && m[1].str()[0] == '"';
    const std::string v = was_string || as_number.empty() ? as_string : as_number;
    json.replace(static_cast<size_t>(m.position(1)), static_cast<size_t>(m.length(1)), v);
  } else {
    const size_t open = json.find('{');
    const bool empty = json.find_first_not_of(" \t\r\n", open + 1) == json.find('}', open);
    json.insert(open + 1, "\"" + key + "\":" + as_string + (empty ? "" : ","));
  }
}

void write_app_storage(long long id, const std::string& name, const std::string& display) {
  std::string json = app_storage_read();
  put_key(json, "UserId", json_quote(std::to_string(id)), std::to_string(id));
  put_key(json, "Username", json_quote(name), "");
  put_key(json, "DisplayName", json_quote(display), "");
  app_storage_write(json);
}

// On Android, NativeUserJavaInterface answers who is signed in (the app's own
// session); Roblox starts signed in when it names a user. From the website
// sign-in, as Mocktail does for its sign-in.
void weblogin_apply_identity() {
  std::ifstream f(account_path());
  std::string line;
  if (!std::getline(f, line) || !cookies_have_login()) return;
  const size_t t = line.find('\t'), t2 = t == std::string::npos ? t : line.find('\t', t + 1);
  const long long id = strtoll(line.c_str(), nullptr, 10);
  if (id <= 0 || t == std::string::npos) return;
  const std::string name = line.substr(t + 1, t2 == std::string::npos ? t2 : t2 - t - 1);
  const std::string display = t2 == std::string::npos ? name : line.substr(t2 + 1);
  const std::string c = "com/roblox/engine/jni/user/NativeUserJavaInterface.";
  set_java_value_int(c + "getUserId()J", id);
  set_java_value_string(c + "getUsername()Ljava/lang/String;", name);
  set_java_value_string(c + "getDisplayName()Ljava/lang/String;", display);
  set_java_value_string(c + "getAlternateName()Ljava/lang/String;", name);
  write_app_storage(id, name, display);
  set_status("Signed in as " + name);
  LOG("signed in as %s (from the website sign-in)\n", name.c_str());
}

void app_storage_set(const std::string& key, const std::string& value) {
  std::string json = app_storage_read();
  put_key(json, key, json_quote(value), "");
  app_storage_write(json);
}

void app_storage_erase(const std::string& key) {
  std::string json = app_storage_read();
  std::regex re;
  std::smatch m;
  if (!std::regex_search(json, m, key_re(key, re))) return;
  size_t a = static_cast<size_t>(m.position(0)), b = a + static_cast<size_t>(m.length(0));
  const size_t after = json.find_first_not_of(" \t\r\n", b);
  if (after != std::string::npos && json[after] == ',') b = after + 1;  // its comma too
  else {
    const size_t before = json.find_last_not_of(" \t\r\n", a - 1);
    if (before != std::string::npos && json[before] == ',') a = before;
  }
  json.erase(a, b - a);
  app_storage_write(json);
}

std::string app_storage_get(const std::string& key) {
  const std::string json = app_storage_read();
  std::regex re;
  std::smatch m;
  if (!std::regex_search(json, m, key_re(key, re))) return {};
  std::string v = m[1].str();
  if (v.size() >= 2 && v.front() == '"') v = v.substr(1, v.size() - 2);
  return v;
}

std::string weblogin_status() {
  std::lock_guard<std::mutex> l(g_mu);
  return g_status;
}

bool weblogin_start() {
  if (g_running.exchange(true)) return false;
  const std::string helper = helper_path();
  if (access(helper.c_str(), X_OK) != 0) {
    set_status("The browser window is missing (make webview)");
    g_running = false;
    return false;
  }
  // A fresh web profile for this sign-in, deleted afterwards.
  const std::string dir = std::filesystem::absolute(profile_dir() + "/weblogin").string();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  int out[2];
  if (pipe2(out, O_CLOEXEC) != 0) {
    g_running = false;
    return false;
  }
  std::vector<std::string> env;
  for (char** e = environ; *e; ++e)
    if (std::string(*e).rfind("TONIC_WEBVIEW_DATA=", 0) != 0) env.push_back(*e);
  env.push_back("TONIC_WEBVIEW_DATA=" + dir);
  std::vector<char*> envp;
  for (auto& s : env) envp.push_back(s.data());
  envp.push_back(nullptr);
  std::vector<std::string> args = {helper, "--url", "https://www.roblox.com/login", "--login", "--title",
                                   "Sign in to Roblox - tonic", "--size", "520x780"};
  std::vector<char*> argv;
  for (auto& s : args) argv.push_back(s.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, out[1], 1);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);  // no commands: it closes itself
  pid_t pid = 0;
  const int r = posix_spawn(&pid, helper.c_str(), &fa, nullptr, argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&fa);
  close(out[1]);
  if (r != 0) {
    close(out[0]);
    set_status("Could not open the browser window");
    g_running = false;
    return false;
  }
  set_status("Sign in on roblox.com in the new window");
  LOG("sign-in window open\n");
  const int from = out[0];
  std::thread([from, pid, dir] {
    std::string buf, session, user, user_line;
    long long expires = 0;
    char tmp[4096];
    for (;;) {
      const ssize_t n = read(from, tmp, sizeof tmp);
      if (n <= 0) break;
      buf.append(tmp, n);
      size_t nl;
      while ((nl = buf.find('\n')) != std::string::npos) {
        std::string line = buf.substr(0, nl);
        buf.erase(0, nl + 1);
        if (line.rfind("session ", 0) == 0) {  // EXPIRES \t VALUE: kept, never logged
          const size_t t = line.find('\t', 8);
          if (t != std::string::npos) {
            expires = strtoll(line.substr(8, t - 8).c_str(), nullptr, 10);
            session = line.substr(t + 1);
          }
        } else if (line.rfind("user ", 0) == 0) {
          user_line = line.substr(5);  // ID \t NAME \t DISPLAY
          const size_t t = user_line.find('\t');
          const size_t t2 = t == std::string::npos ? t : user_line.find('\t', t + 1);
          user = t == std::string::npos ? "" : user_line.substr(t + 1, t2 == std::string::npos ? t2 : t2 - t - 1);
        } else if (line.rfind("log ", 0) == 0) {
          LOG("%s\n", line.c_str() + 4);
        }
        std::fill(line.begin(), line.end(), '\0');
      }
    }
    close(from);
    waitpid(pid, nullptr, 0);
    if (!session.empty()) {
      cookies_save_login(session, expires);
      // Who is signed in, for Roblox's NativeUserJavaInterface at the next start.
      std::ofstream(account_path(), std::ios::trunc) << user_line << "\n";
      std::fill(session.begin(), session.end(), '\0');
      recent_set_owner(user);  // another account: its own recent games
      set_status(user.empty() ? "Signed in" : "Signed in as " + user);
      LOG("signed in%s%s\n", user.empty() ? "" : " as ", user.c_str());
    } else {
      set_status("Sign-in window closed without signing in");
      LOG("closed without signing in\n");
    }
    std::error_code ec2;
    std::filesystem::remove_all(dir, ec2);  // the window's own copy of the session goes
    g_running = false;
  }).detach();
  return true;
}

}  // namespace tonic
