// Host shim: just enough Arduino for Items.cpp and Scheduler.cpp.
#pragma once
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstdint>
#include <cstdarg>
#include <time.h>
#include <algorithm>
#include <ctime>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <vector>
#include <memory>
using std::min;
class String {
public:
  std::string s;
  String() {}
  String(const char* c) { if (c) s = c; }
  String(const std::string& x) : s(x) {}
  String(char c) : s(1, c) {}
  String(unsigned long v) : s(std::to_string(v)) {}
  String(uint32_t v) : s(std::to_string(v)) {}
  String(int v) : s(std::to_string(v)) {}
  String& operator=(const char* c) { s = c ? c : ""; return *this; }
  const char* c_str() const { return s.c_str(); }
  size_t length() const { return s.size(); }
  bool isEmpty() const { return s.empty(); }
  bool concat(const char* c) { s += c; return true; }
  bool concat(const char* c, size_t n) { s.append(c, n); return true; }
  String& operator+=(const String& o) { s += o.s; return *this; }
  char operator[](size_t i) const { return i < s.size() ? s[i] : 0; }
  bool operator==(const String& o) const { return s == o.s; }
  bool operator!=(const String& o) const { return s != o.s; }
  bool endsWith(const char* e) const { size_t n = strlen(e); return s.size() >= n && s.compare(s.size() - n, n, e) == 0; }
  int indexOf(char c, int from = 0) const { auto p = s.find(c, from); return p == std::string::npos ? -1 : (int)p; }
  String substring(int a, int b) const { return String(s.substr(a, b - a)); }
  String substring(int a) const { return String(s.substr(a)); }
  void trim() { size_t a = s.find_first_not_of(" \t\r\n"); size_t b = s.find_last_not_of(" \t\r\n");
                s = a == std::string::npos ? "" : s.substr(a, b - a + 1); }
  bool reserve(size_t n) { s.reserve(n); return true; }
  const char* begin() const { return s.c_str(); } const char* end() const { return s.c_str() + s.size(); }
};
inline String operator+(const String& a, const String& b) { return String(a.s + b.s); }
inline String operator+(const String& a, const char* b) { return String(a.s + b); }
inline String operator+(const char* a, const String& b) { return String(std::string(a) + b.s); }
inline String operator+(const String& a, char b) { return String(a.s + b); }
#include <chrono>
#include <thread>
inline uint32_t millis() { using namespace std::chrono; return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count(); }
inline void delay(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
