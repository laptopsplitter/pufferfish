// fakejni.hpp - a JavaVM/JNIEnv with no Java behind it: plausible handles for
// what the library needs, values from --values, 0/null for the rest.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>
#include <string>

namespace tonic {

// Pointer to pass as JNI_OnLoad's JavaVM* argument.
void* fake_java_vm();

// Marks the calling thread as attached, the way a Java thread calling
// System.loadLibrary is. Call this on the thread that runs JNI_OnLoad.
// Native threads the library creates are not attached until they call
// AttachCurrentThread; until then GetEnv returns JNI_EDETACHED, as on Android.
void jni_attach_current_thread();

// Prints a summary of the classes looked up and natives registered so far.
void print_jni_summary();

// --values file: what "Java" returns. One per line:
//   CLASS.METHOD(ARGS)RET = VALUE    CLASS.FIELD:TYPE = VALUE
//   CLASS#LABEL.... only for the object made with new:LABEL
// VALUE: "text", number, true/false, null, new.
bool load_java_values(const std::string& path, std::string& err);
// Sets KEY (same form) unless --values already did. For values only known at
// run time, such as the window's physical size.
void set_java_value_default(const std::string& key, double value);
// Sets KEY to a floating point value (always; from tonic's settings).
void set_java_value_double(const std::string& key, double value);
// Sets KEY to a String value (always; for answers C++ code decides at run time).
void set_java_value_string(const std::string& key, const std::string& text);
// Implements a Java method in C++: KEY is CLASS.NAME(ARGS)RET as in --values;
// FN gets the object (or null for static) and the raw arguments and returns the
// raw result (0/1 for boolean, a handle for objects). Takes precedence over
// --values. Register before the library runs. Called outside the JNI lock, so
// it may block.
void register_java_method(const std::string& key,
                          std::function<uint64_t(void*, const std::vector<uint64_t>&)> fn);
// The Strings in a String[] the library made.
std::vector<std::string> jni_string_array(void* handle);
// Constructor arguments of an object the library made with NewObject (raw
// bits in signature order; floats as a double's bits). Empty otherwise.
std::vector<uint64_t> jni_ctor_args(void* handle);
// Live Java objects (for the frame-rate log: a count that keeps growing is a leak).
size_t jni_object_count();
// A float[] holding VALUES.
void* jni_new_float_array(const std::vector<float>& values);
// Sets KEY to an integer/boolean (always).
void set_java_value_int(const std::string& key, int64_t v);
// Sets KEY to "new" (a fresh object of the result type) at run time.
void set_java_value_new(const std::string& key);
// A String value from --values ("" if none).
std::string java_value_string(const std::string& key);
// Called (label, method name, raw arguments) whenever the library calls a
// method on an object made with a label. Register before the library runs.
void add_jni_call_listener(
    std::function<void(const std::string&, const std::string&, const std::vector<uint64_t>&)> fn);

// For calling natives the way Java would (src/script.cpp). All handles are
// ours, so the library can pass them back into JNI and we will recognise them.
void* jni_env();  // this thread's JNIEnv; the thread must be attached
void* jni_class(const std::string& name);  // "com/example/Foo"
void* jni_new_object(const std::string& class_name, const std::string& label = {});
// Called (label, method name, logged arguments, the String arguments' text)
// after the library calls a method on an object made with a label, or a static
// method ("static:NAME"). Used for "on LABEL" script rules.
void set_jni_call_hook(std::function<void(const std::string&, const std::string&, const std::string&, const std::vector<std::string>&)> hook);
void* jni_new_string(const std::string& utf);
// Adds an observer (all are called): (method name, raw arguments) for every call the library makes into
// "Java", on the library's thread. Raw arguments are the 64-bit values passed
// (handles, integers; floats and doubles as a double's bits).
void set_jni_observer(std::function<void(const std::string&, const std::vector<uint64_t>&)> fn);
// The contents of a String, or of a byte[] as raw bytes ("" for anything else).
std::string jni_text(void* handle);
std::string jni_describe(void* handle);  // "\"text\"", "null", "object com/x/Y", "class com/x/Y"
// Address of a native registered with RegisterNatives for exactly this class,
// name and descriptor, or 0.
uint64_t jni_registered_native(const std::string& cls, const std::string& name, const std::string& desc);

}  // namespace tonic
