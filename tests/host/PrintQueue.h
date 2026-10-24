#pragma once
#include "JobStore.h"
#include <vector>
struct Sub { uint32_t id; bool raster; std::string text; size_t len; uint16_t rows; bool cut; uint8_t cov, font, align, scale; bool tail; uint8_t first; };
struct PrintQueue {
  bool reach = true; size_t pend = 0; bool bsy = false; uint32_t idle = 0; uint32_t nextId = 100;
  JobStore* js = nullptr; std::vector<Sub> subs;
  bool printerReachable() const { return reach; } size_t pending() const { return pend; } bool busy() const { return bsy; }
  uint32_t idleMs() const { return idle; }
  uint32_t submitRasterOwned(uint8_t* d, size_t len, uint16_t, uint16_t rows, const JobSource&, bool cut, uint16_t, uint8_t cov, const uint8_t*, uint8_t, bool tail) {
    subs.push_back({ nextId, true, "", len, rows, cut, cov, 0, 0, 0, tail, d[0] }); free(d); js->st[nextId] = (uint8_t)JobStatus::Queued; return nextId++; }
  uint32_t submitText(const String& t, const JobSource&, uint8_t align, uint8_t scale, bool, bool cut, uint8_t font) {
    subs.push_back({ nextId, false, t.s, t.length(), 0, cut, 0, font, align, scale, false, 0 }); js->st[nextId] = (uint8_t)JobStatus::Queued; return nextId++; }
};
