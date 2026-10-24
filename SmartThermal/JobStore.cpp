#include "JobStore.h"
#include <LittleFS.h>

static const char* PATH = "/jobs.dat";

bool JobStore::begin() {
  memset(_slots, 0, sizeof(_slots));

  // formatOnFail = true: a blank or corrupt filesystem is formatted rather
  // than leaving history permanently broken.
  if (!LittleFS.begin(true)) {
    _mounted = false;
    return false;
  }
  _mounted = true;

  if (!load()) writeAll();     // absent or wrong-version file: start fresh
  return true;
}

// ----------------------------------------------------------------------
// Persistence
// ----------------------------------------------------------------------

bool JobStore::load() {
  File f = LittleFS.open(PATH, "r");
  if (!f) return false;

  uint8_t header[HEADER_BYTES] = {0};
  if (f.read(header, HEADER_BYTES) != HEADER_BYTES) { f.close(); return false; }

  uint32_t magic;
  memcpy(&magic, header, 4);
  if (magic != MAGIC || header[4] != FORMAT_VERSION) { f.close(); return false; }

  memcpy(&_head,  header + 6, 2);
  memcpy(&_count, header + 8, 2);
  if (_head >= CAPACITY || _count > CAPACITY) { f.close(); return false; }

  const size_t want = sizeof(_slots);
  const size_t got  = f.read(reinterpret_cast<uint8_t*>(_slots), want);
  f.close();
  if (got != want) return false;

  _highestId = 0;
  for (uint16_t i = 0; i < CAPACITY; i++)
    if (_slots[i].id > _highestId) _highestId = _slots[i].id;

  return true;
}

bool JobStore::writeAll() {
  if (!_mounted) return false;

  File f = LittleFS.open(PATH, "w");
  if (!f) return false;

  uint8_t header[HEADER_BYTES] = {0};
  const uint32_t magic = MAGIC;
  memcpy(header,     &magic,  4);
  header[4] = FORMAT_VERSION;
  memcpy(header + 6, &_head,  2);
  memcpy(header + 8, &_count, 2);

  f.write(header, HEADER_BYTES);
  f.write(reinterpret_cast<const uint8_t*>(_slots), sizeof(_slots));
  f.close();
  return true;
}

// Rewrites one record plus the header, rather than the whole 8 KB file.
bool JobStore::flushSlot(uint16_t idx) {
  if (!_mounted || idx >= CAPACITY) return false;

  File f = LittleFS.open(PATH, "r+");
  if (!f) return writeAll();          // file missing: rebuild it

  uint8_t head[4] = {0};
  memcpy(head, &_head, 2);
  memcpy(head + 2, &_count, 2);

  bool ok = f.seek(6) &&
            f.write(head, 4) == 4 &&
            f.seek(HEADER_BYTES + static_cast<size_t>(idx) * sizeof(StoredJob)) &&
            f.write(reinterpret_cast<const uint8_t*>(&_slots[idx]),
                    sizeof(StoredJob)) == sizeof(StoredJob);
  f.close();

  if (!ok) return writeAll();
  return true;
}

// ----------------------------------------------------------------------
// Access
// ----------------------------------------------------------------------

int16_t JobStore::indexOf(uint32_t id) const {
  if (id == 0) return -1;
  for (uint16_t i = 0; i < CAPACITY; i++)
    if (_slots[i].id == id) return static_cast<int16_t>(i);
  return -1;
}

void JobStore::upsert(const StoredJob& job, bool force) {
  int16_t idx = indexOf(job.id);

  if (idx < 0) {
    idx   = _head;
    _head = (_head + 1) % CAPACITY;
    if (_count < CAPACITY) _count++;
  }

  _slots[idx] = job;
  if (job.id > _highestId) _highestId = job.id;

  // 3 = Done, 4 = Failed in JobStatus. Only terminal states hit flash.
  const bool terminal = (job.status == 3 || job.status == 4);
  if (force || terminal) flushSlot(static_cast<uint16_t>(idx));
}

bool JobStore::get(uint32_t id, StoredJob& out) const {
  const int16_t idx = indexOf(id);
  if (idx < 0) return false;
  out = _slots[idx];
  return true;
}

uint16_t JobStore::recent(StoredJob* out, uint16_t max) const {
  uint16_t n = 0;
  for (uint16_t i = 0; i < CAPACITY && n < max; i++) {
    const uint16_t idx = (_head + CAPACITY - 1 - i) % CAPACITY;
    if (_slots[idx].id != 0) out[n++] = _slots[idx];
  }
  return n;
}

uint32_t JobStore::logEvent(uint8_t type, const char* source,
                            const char* note, uint32_t epoch) {
  StoredJob e = {};
  e.id     = _highestId + 1;
  e.epoch  = epoch;
  e.type   = type;
  e.status = 3;                 // Done: an event has no pending state
  strlcpy(e.source, source ? source : "system", sizeof(e.source));
  strlcpy(e.error,  note   ? note   : "",       sizeof(e.error));

  upsert(e, true);
  return e.id;
}

void JobStore::clear() {
  memset(_slots, 0, sizeof(_slots));
  _head  = 0;
  _count = 0;
  // _highestId is deliberately kept, so cleared history does not cause new
  // jobs to reuse old numbers.
  writeAll();
}
