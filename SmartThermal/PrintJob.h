// PrintJob.h — Section 18.
//
// A job is a self-contained description of something to print, with its
// payload on the heap and owned by the job. Once submitted, the submitting
// task must not touch it again — the printer task owns it and frees it.

#pragma once
#include <Arduino.h>
#include <IPAddress.h>

enum class JobType : uint8_t {
  Text   = 0,
  Raster = 1,
  Raw    = 2,     // arbitrary ESC/POS, dev use only
  Event  = 3      // not a print: a boot or other system event, logged so
                  // the history explains gaps rather than just showing one
};

enum class JobStatus : uint8_t {
  Unknown = 0,
  Queued,
  Printing,
  Done,
  Failed
};

inline const char* jobTypeName(JobType t) {
  switch (t) {
    case JobType::Text:   return "text";
    case JobType::Raster: return "raster";
    case JobType::Raw:    return "raw";
    case JobType::Event:  return "event";
  }
  return "unknown";
}

inline const char* jobStatusName(JobStatus s) {
  switch (s) {
    case JobStatus::Queued:   return "queued";
    case JobStatus::Printing: return "printing";
    case JobStatus::Done:     return "done";
    case JobStatus::Failed:   return "failed";
    default:                  return "unknown";
  }
}

// Where a job came from. Attached at submission, because by the time the
// printer task runs the job the HTTP connection is long gone.
struct JobSource {
  const char* label;      // "web (iPhone)", "curl", "serial", ...
  IPAddress   ip;         // 0.0.0.0 for locally submitted jobs

  static JobSource local(const char* label) {
    return { label, IPAddress(0, 0, 0, 0) };
  }
};

struct PrintJob {
  uint32_t id;
  JobType  type;
  uint32_t createdMs;
  uint32_t epoch;         // unix seconds at submission, 0 if clock unset
  char     source[24];
  uint8_t  sourceIp[4];

  uint8_t* payload;      // malloc'd. Text is NUL-terminated; raster is packed 1bpp.
  size_t   payloadLen;

  // Text options
  uint8_t  align;        // 0 left, 1 centre, 2 right
  uint8_t  scale;        // 1..8
  bool     bold;
  uint8_t  font;         // 0 = Font A (12x24 cell), 1 = Font B (9x17)
  uint8_t  pre[8];       // raster: raw commands sent after the job's ESC @
  uint8_t  preLen;       //   (bench tests; a separate job would be reset)
  bool     noTail;       // raster: skip the white tail (bench, raw panels)

  // Raster options
  uint16_t widthDots;
  uint16_t rows;

  // Percentage of black dots in the packed raster, 0-100. This is the one
  // number that decides how long the printer must rest afterwards: it
  // faults when coverage% x duty% exceeds roughly 1000, so the queue uses
  // it to compute the gap before the next job.
  //
  // The browser counts the set bits while packing and sends the figure with
  // the upload, because it has the buffer in hand already. 0 means "not
  // supplied" — the printer task then counts the bits itself rather than
  // assuming a safe value, so a job posted by curl is paced too.
  uint8_t  coverage;
  // Rows per GS v 0 command for THIS job. 0 follows the printer setting.
  // Per-job so a diagnostic can send an image as one single command
  // without disturbing the saved configuration.
  uint16_t chunkRows;

  // Common
  bool     cut;
};
