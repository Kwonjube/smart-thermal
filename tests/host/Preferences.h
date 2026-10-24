#pragma once
#include <Arduino.h>
#include <map>
extern std::map<std::string, std::string> fake_nvs;
struct Preferences { std::string ns;
  bool begin(const char* n, bool) { ns = n; return true; } void end() {}
  String getString(const char* k, const char* d) { auto it = fake_nvs.find(ns + "/" + k); return it == fake_nvs.end() ? String(d) : String(it->second); }
  size_t putString(const char* k, const String& v) { fake_nvs[ns + "/" + k] = v.s; return v.length(); } };
