#include "Items.h"
#include "Log.h"
#include <LittleFS.h>

const uint8_t ItemStore::LIMIT[ItemStore::KINDS] = { 10, 20, 8 };

static const char* DIR       = "/k";
static const char* PIC_PATH  = "/k/pic.jpg";
static const char* PIC_OWNER = "/k/pic.txt";

bool ItemStore::kindFrom(char c, Kind& k) {
  switch (c) {
    case 'd': k = DRAFT; return true;
    case 's': k = SAVED; return true;
    case 'p': k = SCHED; return true;
  }
  return false;
}

String ItemStore::path(Kind k, uint32_t id, const char* ext) const {
  return String(DIR) + "/" + kindChar(k) + String(id) + ext;
}

bool ItemStore::begin() {
  if (!LittleFS.exists(DIR) && !LittleFS.mkdir(DIR)) {
    Log.println("[items] cannot create /k - drafts, saved and scheduled are off");
    return false;
  }
  _ok = true;
  scan();
  // The picture is only kept while the item that owns it exists.
  if (LittleFS.exists(PIC_OWNER)) {
    File f = LittleFS.open(PIC_OWNER, "r");
    _picOwner = f ? f.readString() : String();
    f.close();
    _picOwner.trim();
    Kind k; uint32_t id = _picOwner.length() > 1 ? strtoul(_picOwner.c_str() + 1, nullptr, 10) : 0;
    if (_picOwner.isEmpty() || !kindFrom(_picOwner[0], k) || !exists(k, id) || !LittleFS.exists(PIC_PATH)) {
      Log.println("[items] stored picture has no owner - removed");
      dropPic();
    }
  } else if (LittleFS.exists(PIC_PATH)) {
    LittleFS.remove(PIC_PATH);
  }
  Log.printf("[items] %u drafts, %u saved, %u scheduled, %u KB free%s\n",
             _count[DRAFT], _count[SAVED], _count[SCHED], (unsigned)(freeBytes() / 1024),
             _picOwner.length() ? ", 1 picture" : "");
  return true;
}

// Counts files per kind and finds the highest id. Leftover .bin files
// without a description (an interrupted upload) are removed.
void ItemStore::scan() {
  _count[0] = _count[1] = _count[2] = 0;
  File dir = LittleFS.open(DIR);
  if (!dir) return;
  String orphans;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    const String n = f.name();
    f.close();
    Kind k;
    if (n.length() < 3 || !kindFrom(n[0], k)) continue;
    const uint32_t id = strtoul(n.c_str() + 1, nullptr, 10);
    if (!id) continue;
    if (id > _maxId) _maxId = id;
    if (n.endsWith(".json")) _count[k]++;
    else if (n.endsWith(".bin") && !LittleFS.exists(path(k, id))) orphans += path(k, id, ".bin") + "\n";
  }
  dir.close();
  int from = 0;
  while (from < (int)orphans.length()) {
    const int nl = orphans.indexOf('\n', from);
    LittleFS.remove(orphans.substring(from, nl));
    from = nl + 1;
  }
}

uint8_t ItemStore::ids(Kind k, uint32_t* out, uint8_t max) const {
  uint8_t n = 0;
  File dir = LittleFS.open(DIR);
  if (!dir) return 0;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    const String nm = f.name();
    f.close();
    if (nm.length() < 7 || nm[0] != kindChar(k) || !nm.endsWith(".json")) continue;
    const uint32_t id = strtoul(nm.c_str() + 1, nullptr, 10);
    if (!id) continue;
    // Insertion sort, newest (highest id) first.
    uint8_t i = n < max ? n++ : max;
    if (i == max) { if (id < out[max - 1]) continue; i = max - 1; }
    while (i > 0 && out[i - 1] < id) { out[i] = out[i - 1]; i--; }
    out[i] = id;
  }
  dir.close();
  return n;
}

bool ItemStore::exists(Kind k, uint32_t id) const {
  return id && LittleFS.exists(path(k, id));
}

bool ItemStore::writeJson(Kind k, uint32_t id, const String& json, bool isNew) {
  if (!_ok || json.length() > MAX_JSON) return false;
  // Write beside, then swap: a power cut mid-write never leaves half a file.
  const String p = path(k, id), tmp = p + ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;
  const size_t n = f.print(json);
  f.close();
  if (n != json.length()) { LittleFS.remove(tmp); return false; }
  LittleFS.remove(p);
  if (!LittleFS.rename(tmp, p)) { LittleFS.remove(tmp); return false; }
  if (isNew) _count[k]++;
  _ver++;
  return true;
}

bool ItemStore::readJson(Kind k, uint32_t id, String& out) const {
  File f = LittleFS.open(path(k, id), "r");
  if (!f) return false;
  out = f.readString();
  f.close();
  return out.length() > 0;
}

bool ItemStore::remove(Kind k, uint32_t id) {
  if (!exists(k, id)) return false;
  LittleFS.remove(path(k, id));
  LittleFS.remove(path(k, id, ".bin"));
  if (_count[k]) _count[k]--;
  _ver++;
  if (_picOwner == String(kindChar(k)) + String(id)) dropPic();
  return true;
}

bool ItemStore::setPicOwner(const String& owner) {
  File f = LittleFS.open(PIC_OWNER, "w");
  if (!f) return false;
  f.print(owner);
  f.close();
  _picOwner = owner;
  return true;
}

void ItemStore::dropPic() {
  LittleFS.remove(PIC_PATH);
  LittleFS.remove(PIC_OWNER);
  _picOwner = "";
}

size_t ItemStore::freeBytes() const {
  const size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
  return used < total ? total - used : 0;
}
