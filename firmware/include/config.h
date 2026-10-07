#pragma once
#include <stddef.h>
#include <stdint.h>

#define FW_VERSION "0.1.0"
#define DEVICE_ID  "rtd-logger-01"  // unique per unit; shows up as the device name on the dashboard

// ---- Wiring (spec section 05) -------------------------------------------------------------
constexpr int PIN_SCK  = 12;  // FSPI IOMUX pins
constexpr int PIN_MOSI = 11;
constexpr int PIN_MISO = 13;
constexpr int PIN_SDA  = 8;   // DS3231
constexpr int PIN_SCL  = 9;
constexpr int PIN_CS[8] = {4, 5, 6, 7, 15, 16, 17, 18};  // CS1..CS8 -> channels 1..8

// ---- RTDs ---------------------------------------------------------------------------------
// Channels 1-7 are PT100, channel 8 is PT1000. R_REF must be the value actually fitted on each
// module: Adafruit-style boards use 430 ohm (PT100) and 4300 ohm (PT1000). The spec's bare-IC
// design uses 4020 ohm for PT1000, so change this if you move to a custom board.
constexpr bool  CH_IS_PT1000[8] = {false, false, false, false, false, false, false, true};
constexpr float R_NOMINAL_PT100  = 100.0f;
constexpr float R_NOMINAL_PT1000 = 1000.0f;
constexpr float R_REF_PT100      = 430.0f;
constexpr float R_REF_PT1000     = 4300.0f;
constexpr int   MAINS_HZ         = 50;  // 50 or 60: sets the MAX31865 notch filter

// ---- Timing -------------------------------------------------------------------------------
constexpr uint32_t SAMPLE_INTERVAL_MS = 10UL * 1000UL;
constexpr uint32_t NTP_RESYNC_MS      = 24UL * 60UL * 60UL * 1000UL;
constexpr uint32_t HTTP_TIMEOUT_MS    = 4000;
constexpr uint32_t WIFI_RETRY_MS      = 30UL * 1000UL;
constexpr const char* NTP_SERVER_1 = "pool.ntp.org";
constexpr const char* NTP_SERVER_2 = "time.google.com";
constexpr uint32_t MIN_VALID_EPOCH = 1704067200UL;  // 2024-01-01; anything older means "clock not set"

// ---- Offline backlog ----------------------------------------------------------------------
// 40 bytes per row, so 40000 rows is 1.6 MB of the 2 MB PSRAM: roughly 111 hours at 10 s.
// Without PSRAM (some dev boards) it falls back to a small internal-RAM queue.
constexpr size_t QUEUE_CAPACITY_PSRAM    = 40000;
constexpr size_t QUEUE_CAPACITY_INTERNAL = 1500;

// Every row that could not be uploaded is also written to flash, so a power cut does not lose it.
// Rows live in segment files on the 2 MB "spiffs" partition (LittleFS), which holds up to
// 36000 rows (1.4 MB, about 100 hours). Beyond that the oldest segment is deleted.
constexpr uint32_t FLASH_SEG_ROWS = 400;  // rows per file, about 67 minutes
constexpr size_t   FLASH_MAX_SEGS = 90;
