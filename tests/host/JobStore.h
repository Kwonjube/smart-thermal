#pragma once
#include "PrintJob.h"
#include <map>
struct StoredJob { uint32_t id; uint8_t status; };
struct JobStore { std::map<uint32_t, uint8_t> st;
  bool get(uint32_t id, StoredJob& o) const { auto i = st.find(id); if (i == st.end()) return false; o = { id, i->second }; return true; } };
