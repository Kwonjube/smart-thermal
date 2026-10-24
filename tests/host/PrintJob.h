#pragma once
#include <Arduino.h>
enum class JobStatus : uint8_t { Unknown = 0, Queued, Printing, Done, Failed };
struct JobSource { const char* label; static JobSource local(const char* l) { return { l }; } };
