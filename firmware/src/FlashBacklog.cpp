#include "FlashBacklog.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>

#include <algorithm>
#include <vector>

#include "config.h"

namespace {
const char* kDir = "/bl1";  // bump the suffix if the Reading layout ever changes

Preferences prefs;
bool ready = false;
std::vector<uint32_t> segs;           // first row id of each segment file, oldest first
uint32_t curRows = FLASH_SEG_ROWS;    // "full", so the first append after boot starts a fresh file
uint32_t consumed = 0;                // every row id up to here has reached the server
uint32_t newest = 0;

String segPath(uint32_t first) { return String(kDir) + "/" + String(first) + ".bin"; }

void dropOldestSegment() {
  if (segs.empty()) return;
  LittleFS.remove(segPath(segs.front()));
  segs.erase(segs.begin());
}
}  // namespace

namespace FlashBacklog {

bool begin(ReadingQueue& q) {
  if (!LittleFS.begin(true, "/littlefs", 10, "spiffs")) {
    Serial.println("flash backlog: mount failed, running from RAM only");
    return false;
  }
  prefs.begin("rtdlog", false);
  consumed = prefs.getUInt("consumed", 0);
  newest = consumed;
  if (!LittleFS.exists(kDir)) LittleFS.mkdir(kDir);

  std::vector<uint32_t> found;
  File dir = LittleFS.open(kDir);
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!f.isDirectory()) found.push_back((uint32_t)strtoul(f.name(), nullptr, 10));
  }
  dir.close();
  std::sort(found.begin(), found.end());

  size_t restored = 0;
  for (uint32_t first : found) {
    File f = LittleFS.open(segPath(first), FILE_READ);
    if (!f) continue;
    Reading r;
    uint32_t maxId = 0;
    // A row cut short by a power loss is smaller than a full record and is simply not read.
    while (f.read((uint8_t*)&r, sizeof(r)) == sizeof(r)) {
      if (r.id > maxId) maxId = r.id;
      if (r.id > consumed) {
        q.push(r);
        restored++;
      }
    }
    f.close();
    if (maxId <= consumed) {  // delivered already (or empty), just tidy it away
      LittleFS.remove(segPath(first));
      continue;
    }
    segs.push_back(first);
    if (maxId > newest) newest = maxId;
  }
  ready = true;
  Serial.printf("flash backlog: restored %u unsent rows from %u file(s)\n", (unsigned)restored,
                (unsigned)segs.size());
  return true;
}

uint32_t lastId() { return newest; }

void append(const Reading& r) {
  if (!ready) return;
  if (segs.empty() || curRows >= FLASH_SEG_ROWS) {
    segs.push_back(r.id);
    curRows = 0;
    while (segs.size() > FLASH_MAX_SEGS) dropOldestSegment();
  }
  for (int attempt = 0; attempt < 2; attempt++) {
    File f = LittleFS.open(segPath(segs.back()), FILE_APPEND);
    bool ok = f && f.write((const uint8_t*)&r, sizeof(r)) == sizeof(r);
    if (f) f.close();
    if (ok) {
      curRows++;
      newest = r.id;
      return;
    }
    // Out of space: sacrifice the oldest file (never the one being written) and try once more.
    if (segs.size() < 2) break;
    dropOldestSegment();
  }
  Serial.println("flash backlog: write failed, row is only in RAM");
}

void consumeThrough(uint32_t id) {
  if (!ready || id <= consumed) return;
  consumed = id;
  prefs.putUInt("consumed", id);
  // A file is finished once the next file starts at or below consumed + 1.
  while (segs.size() > 1 && segs[1] - 1 <= consumed) dropOldestSegment();
  if (!segs.empty() && consumed >= newest) {  // queue fully drained
    while (!segs.empty()) dropOldestSegment();
    curRows = FLASH_SEG_ROWS;
  }
}

}  // namespace FlashBacklog
