#include "TimeSync.h"

#include <Arduino.h>
#include <RTClib.h>
#include <Wire.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include <time.h>

#include "config.h"

namespace {
RTC_DS3231 rtc;
bool rtcPresent = false;
volatile bool ntpFresh = false;

// Runs on the lwIP thread, so only flag it; the I2C write happens in poll().
void onNtpSync(struct timeval*) { ntpFresh = true; }
}  // namespace

namespace TimeSync {

bool valid() { return time(nullptr) >= (time_t)MIN_VALID_EPOCH; }

void begin() {
  Wire.begin(PIN_SDA, PIN_SCL);
  rtcPresent = rtc.begin(&Wire);
  if (!rtcPresent) {
    Serial.println("RTC: not found, relying on NTP");
    return;
  }
  if (rtc.lostPower()) {
    Serial.println("RTC: lost power, waiting for NTP");
    return;
  }
  struct timeval tv = {(time_t)rtc.now().unixtime(), 0};
  settimeofday(&tv, nullptr);
  Serial.printf("RTC: clock set to %lu\n", (unsigned long)tv.tv_sec);
}

void startNtp() {
  sntp_set_sync_interval(NTP_RESYNC_MS);  // must be set before SNTP starts
  sntp_set_time_sync_notification_cb(onNtpSync);
  configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);
}

void poll() {
  if (!ntpFresh) return;
  ntpFresh = false;
  Serial.printf("NTP: synced, clock now %lu\n", (unsigned long)time(nullptr));
  if (rtcPresent) rtc.adjust(DateTime((uint32_t)time(nullptr)));
}

}  // namespace TimeSync
