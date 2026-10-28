// script.hpp - call the library's Java natives as the app's Java side would.
// One per line:  [static] CLASS.METHOD(DESC)RET [ARG ...] [=> EXPECTED]
//   ARG: "text", number, true/false, null, new, new:LABEL, file:PATH
// A rule:        on LABEL|static:METHOD[~TEXT] <call>   ($arg1.. = the String args)
// runs the call whenever the library calls into that object or static method.
// # starts a comment. tools/dexnatives.py --script prints lines.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "elfimg.hpp"

namespace tonic {

struct NativeCall {
  bool is_static = false;
  std::string cls, name, desc;  // "com/x/Y", "foo", "(ILjava/lang/String;)V"
  struct Arg {
    std::string text;
    bool quoted;
  };
  std::vector<Arg> args;
  bool has_expect = false;
  std::string expect;
  std::string where;  // "file:line" or "--native", for messages
  std::string on;     // non-empty: an "on LABEL" rule, run from the call hook
  std::string builtin;  // non-empty: a "tonic NAME [ARG ...]" line (register_script_builtin)
  std::vector<std::string> builtin_args;
};

// "tonic NAME" in a script runs FN at that point of the sequence: for steps
// the probe itself performs where the Android app's Java side would (e.g.
// "tonic webview": set up the WebView protocol before the app starts).
void register_script_builtin(const std::string& name,
                             std::function<void(const ElfImage&, const std::vector<std::string>&)> fn);

bool parse_native_call(const std::string& line, const std::string& where, NativeCall& out, std::string& err);
// Appends the calls in FILE. Returns false (with err) on the first bad line.
bool load_script(const std::string& path, std::vector<NativeCall>& out, std::string& err);

// Runs the calls in order on the calling thread, which must be attached.
// Returns the number that failed (not found, bad arguments, unexpected result).
int run_native_calls(const ElfImage& img, const std::vector<NativeCall>& calls);

// Makes the "on LABEL" rules in CALLS fire when the library calls back into an
// object with that label. Rules are queued and run, in order, on a worker
// thread of ours (never inside the library's callback).
void install_rules(const ElfImage& img, const std::vector<NativeCall>& calls);

// A Java native's code: registered with RegisterNatives, else exported as
// Java_<class>_<method> or the long (overloaded) name. Null if none.
void* find_native(const ElfImage& img, const std::string& cls, const std::string& name, const std::string& desc);

// Java_<class>_<method> and the overloaded form Java_<class>_<method>__<args>.
std::string jni_short_name(const std::string& cls, const std::string& name);
std::string jni_long_name(const std::string& cls, const std::string& name, const std::string& desc);

// ${NAME} in a script's quoted strings and file: paths becomes VALUE.
void set_script_var(const std::string& name, const std::string& value);

}  // namespace tonic
