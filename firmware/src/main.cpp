#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include "FlashBacklog.h"
#include "ReadingQueue.h"
#include "Sensors.h"
#include "TimeSync.h"
#include "Uploader.h"
#include "config.h"
#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy include/secrets.example.h to include/secrets.h and fill in Wi-Fi and server details"
#endif

static ReadingQueue backlog;
static uint32_t nextRowId;
static uint32_t nextSampleMs;
static uint32_t lastWifiAttemptMs;

static void initBacklog() {
  size_t cap = QUEUE_CAPACITY_INTERNAL;
  Reading* mem = nullptr;
  if (psramFound()) {
    cap = QUEUE_CAPACITY_PSRAM;
    mem = (Reading*)ps_malloc(cap * sizeof(Reading));
  }
  if (!mem) {
    cap = QUEUE_CAPACITY_INTERNAL;
    mem = (Reading*)malloc(cap * sizeof(Reading));
  }
  backlog.begin(mem, mem ? cap : 0);
  Serial.printf("backlog: %u rows (%u KB) in %s\n", (unsigned)backlog.capacity(),
                (unsigned)(backlog.capacity() * sizeof(Reading) / 1024),
                psramFound() ? "PSRAM" : "internal RAM");
}

static void queueRow(const Reading& r) {
  backlog.push(r);
  FlashBacklog::append(r);
}

static void rowDelivered() {
  Reading oldest;
  if (!backlog.peekOldest(oldest)) return;
  backlog.popOldest();
  FlashBacklog::consumeThrough(oldest.id);
}

// WiFi's own auto-reconnect only covers a link that was up; this also recovers from a failed boot.
static void keepWifiUp() {
  if (WiFi.status() == WL_CONNECTED) return;
  uint32_t now = millis();
  if (now - lastWifiAttemptMs < WIFI_RETRY_MS) return;
  lastWifiAttemptMs = now;
  Serial.println("wifi: connecting");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

static void runCycle() {
  if (!TimeSync::valid()) {
    Serial.println("waiting for a valid clock (RTC or NTP), skipping sample");
    return;
  }

  Reading live{};
  live.id = nextRowId++;
  live.ts = (uint32_t)time(nullptr);
  int good = Sensors::sample(live);

  // The live row always goes out with the oldest backlogged row, so a reconnect catches up
  // one row per cycle instead of flooding the server.
  Reading batch[2];
  size_t n = 0;
  bool haveBacklog = false;
  const char* outcome = "offline, queued";

  if (WiFi.status() == WL_CONNECTED) {
    haveBacklog = backlog.peekOldest(batch[0]);
    if (haveBacklog) n = 1;
    batch[n++] = live;
    size_t left = backlog.size() - (haveBacklog ? 1 : 0);
    Uploader::Result r = Uploader::post(batch, n, left);
    if (r == Uploader::Result::Retry) {
      queueRow(live);
      outcome = "upload failed, queued";
    } else {
      if (haveBacklog) rowDelivered();
      outcome = r == Uploader::Result::Ok ? "sent" : "dropped by server";
    }
  } else {
    queueRow(live);
  }

  Serial.printf("%lu ok=%d/%d [", (unsigned long)live.ts, good, kChannels);
  for (int ch = 0; ch < kChannels; ch++) {
    if (isnan(live.temp_c[ch])) Serial.print(" --");
    else Serial.printf(" %.2f", live.temp_c[ch]);
  }
  Serial.printf(" ] %s, backlog %u (dropped %lu)\n", outcome, (unsigned)backlog.size(),
                (unsigned long)backlog.dropped());
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\nRTD logger %s, device %s\n", FW_VERSION, DEVICE_ID);

  initBacklog();
  FlashBacklog::begin(backlog);  // brings back anything that was unsent before a reset or power cut
  nextRowId = FlashBacklog::lastId() + 1;
  Sensors::begin();
  TimeSync::begin();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiAttemptMs = millis();
  TimeSync::startNtp();

  nextSampleMs = millis();
}

void loop() {
  keepWifiUp();
  TimeSync::poll();

  uint32_t now = millis();
  if ((int32_t)(now - nextSampleMs) >= 0) {
    nextSampleMs += SAMPLE_INTERVAL_MS;
    if ((int32_t)(now - nextSampleMs) >= 0) nextSampleMs = now + SAMPLE_INTERVAL_MS;  // fell behind
    runCycle();
  }
  delay(20);
}
