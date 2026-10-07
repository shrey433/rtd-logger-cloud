#pragma once
#include <stdint.h>

#include "ReadingQueue.h"

// Flash copy of the rows that have not reached the server yet. The RAM queue stays the working
// copy; this only exists so a power cut or reset does not lose the backlog. Writes happen only
// while rows are being queued, so a healthy connection does not wear the flash.
namespace FlashBacklog {
// Mounts LittleFS and pushes every unsent row from the last run back into `q`.
// Returns false if flash is unavailable, in which case everything keeps working from RAM only.
bool begin(ReadingQueue& q);

// Highest row id seen so far, so new rows can continue the numbering after a reboot.
uint32_t lastId();

// Persist a row that is being queued.
void append(const Reading& r);

// The server has stored every row up to and including `id`; release their flash space.
void consumeThrough(uint32_t id);
}  // namespace FlashBacklog
