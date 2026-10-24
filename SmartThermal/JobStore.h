// JobStore.h — persistent print history.
//
// 100 fixed-size records held in RAM (~8 KB) and mirrored to a single
// LittleFS file, so history survives a reboot or reflash. Oldest record is
// overwritten once full.
//
// Flash is only written when a job reaches a terminal state (done or
// failed). Transient states live in RAM only. That keeps it to one small
// write per job rather than three, which matters for flash wear: at one
// write per print, a 100-slot ring sees each sector rewritten roughly once
// per 100 jobs.

#pragma once
#include <Arduino.h>
#include <IPAddress.h>

// Packed deliberately: this struct is the on-flash format. Changing it
// requires bumping FORMAT_VERSION, which invalidates the stored file.
struct StoredJob {
  uint32_t id;
  uint32_t epoch;          // unix seconds at submission, 0 if clock unset
  uint32_t durationMs;
  uint32_t bytes;
  uint8_t  type;           // JobType
  uint8_t  status;         // JobStatus
  uint8_t  sourceIp[4];    // 0.0.0.0 for locally submitted jobs
  char     source[24];     // "web (iPhone)", "curl", "serial", ...
  char     error[32];
} __attribute__((packed));

class JobStore {
public:
  static const uint16_t CAPACITY       = 100;
  static const uint32_t MAGIC          = 0x544A4C31;  // "TJL1"
  static const uint8_t  FORMAT_VERSION = 1;

  // Mounts LittleFS (formatting on first run) and loads any existing log.
  bool begin();

  // Insert or update by job id. Only flushed to flash when the status is
  // terminal, or when force is true.
  void upsert(const StoredJob& job, bool force = false);

  bool     get(uint32_t id, StoredJob& out) const;
  uint16_t recent(StoredJob* out, uint16_t max) const;   // newest first
  uint16_t count() const { return _count; }

  // Highest id ever stored, so job numbering continues across a reboot
  // instead of restarting at 1 and colliding with old records.
  uint32_t highestId() const { return _highestId; }

  // Records a non-print event (a reboot, say) so history gaps are
  // explained. Allocates the next id and flushes immediately.
  uint32_t logEvent(uint8_t type, const char* source, const char* note,
                    uint32_t epoch);

  void clear();
  bool mounted() const { return _mounted; }

private:
  static const size_t HEADER_BYTES = 16;

  StoredJob _slots[CAPACITY];
  uint16_t  _head      = 0;     // next slot to write
  uint16_t  _count     = 0;
  uint32_t  _highestId = 0;
  bool      _mounted   = false;

  int16_t indexOf(uint32_t id) const;
  bool    load();
  bool    writeAll();
  bool    flushSlot(uint16_t idx);
};
