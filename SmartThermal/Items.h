// Items.h - drafts, saved prints and scheduled prints, kept on the unit so
// every phone sees the same lists (v0.19).
//
// One small JSON file per item in /k on LittleFS:
//   /k/d<id>.json   draft      (limit 10) - "Keep as draft", never named
//   /k/s<id>.json   saved      (limit 20) - "Save", reloaded or quick-printed
//   /k/p<id>.json   scheduled  (limit 8)  - plus /k/p<id>.bin, the slip
//                                           already drawn by the phone
//   /k/pic.jpg      the ONE stored picture (owner's choice: one at a time),
//   /k/pic.txt      and the item that owns it, e.g. "s12"
//
// Drafts and saved items hold the Text card's contents (style, text, form
// wording), never pixels, so they stay small. A scheduled print also keeps
// the finished slip, because nobody's phone is open when it fires.
//
// Everything here runs on loop() (web handlers and the scheduler), so
// there is no locking.

#pragma once
#include <Arduino.h>

class ItemStore {
public:
  enum Kind : uint8_t { DRAFT = 0, SAVED = 1, SCHED = 2, KINDS = 3 };
  static const uint8_t  LIMIT[KINDS];
  static const size_t   MAX_JSON   = 8 * 1024;    // one item's description
  static const size_t   MAX_PIC    = 350 * 1024;  // the one picture
  static const size_t   RESERVE    = 64 * 1024;   // always left free

  bool begin();                          // after LittleFS is mounted
  bool ok() const { return _ok; }

  static bool kindFrom(char c, Kind& k);
  static char kindChar(Kind k) { return "dsp"[k]; }

  uint8_t  count(Kind k) const { return _count[k]; }
  bool     full(Kind k)  const { return _count[k] >= LIMIT[k]; }
  uint32_t newId()             { return ++_maxId; }
  // Bumped on every change, so pages know when to reload their lists.
  uint32_t version()     const { return _ver; }

  // Newest first. Returns how many ids were written.
  uint8_t  ids(Kind k, uint32_t* out, uint8_t max) const;
  bool     exists(Kind k, uint32_t id) const;

  String   path(Kind k, uint32_t id, const char* ext = ".json") const;
  bool     writeJson(Kind k, uint32_t id, const String& json, bool isNew);
  bool     readJson(Kind k, uint32_t id, String& out) const;
  bool     remove(Kind k, uint32_t id);  // also its .bin and the picture it owns

  // The picture slot.
  const String& picOwner() const { return _picOwner; }   // "" or e.g. "s12"
  bool     picFree() const { return _picOwner.isEmpty(); }
  bool     setPicOwner(const String& owner);
  void     dropPic();

  size_t   freeBytes() const;            // minus nothing; compare with RESERVE

private:
  bool     _ok = false;
  uint8_t  _count[KINDS] = {0, 0, 0};
  uint32_t _maxId = 0;
  uint32_t _ver   = 1;
  String   _picOwner;

  void scan();
};
