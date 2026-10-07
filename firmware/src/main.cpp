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
  // one row per cycle instead of flooding the server. Each is its own acknowledged message.
  const char* outcome = "offline, queued";

  if (Uploader::ready()) {
    bool linkOk = true;
    Reading oldest;
    if (backlog.peekOldest(oldest)) {
      if (Uploader::publish(oldest, backlog.size() - 1) == Uploader::Result::Ok) rowDelivered();
      else linkOk = false;  // no point waiting for a second timeout on a dead link
    }
    if (linkOk && Uploader::publish(live, backlog.size()) == Uploader::Result::Ok) {
      outcome = "sent";
    } else {
      queueRow(live);
      outcome = "upload failed, queued";
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
  Sensors::selfTest();
  TimeSync::begin();

  WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP)
      Serial.printf("wifi: connected to %s, ip %s\n", WIFI_SSID, WiFi.localIP().toString().c_str());
    else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
      Serial.printf("wifi: not connected (reason %d%s)\n", info.wifi_sta_disconnected.reason,
                    info.wifi_sta_disconnected.reason == WIFI_REASON_NO_AP_FOUND
                        ? ": network not seen, the ESP32 needs a 2.4 GHz network"
                        : info.wifi_sta_disconnected.reason == WIFI_REASON_AUTH_FAIL ? ": wrong password" : "");
  });
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiAttemptMs = millis();
  TimeSync::startNtp();
  Uploader::begin();

  nextSampleMs = millis();
}

void loop() {
  keepWifiUp();
  TimeSync::poll();
  Uploader::poll();

  uint32_t now = millis();
  if ((int32_t)(now - nextSampleMs) >= 0) {
    nextSampleMs += SAMPLE_INTERVAL_MS;
    if ((int32_t)(now - nextSampleMs) >= 0) nextSampleMs = now + SAMPLE_INTERVAL_MS;  // fell behind
    runCycle();
  }
  delay(20);
}
