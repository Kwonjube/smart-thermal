#pragma once
#include <Arduino.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <memory>
namespace fs_ = std::filesystem;
extern std::string fake_root;
class File {
public:
  std::shared_ptr<std::fstream> f; std::string path; bool dir = false;
  std::shared_ptr<fs_::directory_iterator> it;
  explicit operator bool() const { return (f && f->is_open()) || dir; }
  void close() { if (f) f->close(); f.reset(); dir = false; }
  String readString() { std::stringstream ss; ss << f->rdbuf(); return String(ss.str()); }
  size_t print(const String& s) { f->write(s.c_str(), s.length()); return f->good() ? s.length() : 0; }
  size_t write(const uint8_t* b, size_t n) { f->write((const char*)b, n); return f->good() ? n : 0; }
  size_t read(uint8_t* b, size_t n) { f->read((char*)b, n); return (size_t)f->gcount(); }
  bool seek(uint32_t o) { f->seekg(o); return f->good(); }
  String name() const { return String(fs_::path(path).filename().string()); }
  File openNextFile() { File r; if (!it) it = std::make_shared<fs_::directory_iterator>(path);
    if (*it == fs_::directory_iterator()) return r; r.path = (*it)->path().string(); r.f = std::make_shared<std::fstream>(r.path, std::ios::in | std::ios::binary); ++(*it); return r; }
};
struct FakeFS {
  std::string p(const String& x) const { return fake_root + x.s; }
  bool exists(const String& x) const { return fs_::exists(p(x)); }
  bool mkdir(const String& x) { return fs_::create_directories(p(x)) || exists(x); }
  bool remove(const String& x) { return fs_::remove(p(x)); }
  bool rename(const String& a, const String& b) { std::error_code e; fs_::rename(p(a), p(b), e); return !e; }
  File open(const String& x, const char* mode = "r") { File r; r.path = p(x);
    if (fs_::is_directory(r.path)) { r.dir = true; return r; }
    if (mode[0] == 'r' && !fs_::exists(r.path)) return r;
    r.f = std::make_shared<std::fstream>(r.path, mode[0] == 'w' ? (std::ios::out | std::ios::binary | std::ios::trunc) : (std::ios::in | std::ios::binary));
    return r; }
  size_t totalBytes() const { return 1024 * 1024; }
  size_t usedBytes() const { size_t n = 0; for (auto& e : fs_::recursive_directory_iterator(fake_root)) if (e.is_regular_file()) n += e.file_size(); return n; }
};
extern FakeFS LittleFS;
