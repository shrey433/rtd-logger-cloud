#pragma once
#include <stdint.h>

namespace TimeSync {
// Sets the system clock from the DS3231 if one is fitted and holds a valid time.
void begin();

// Starts SNTP: it syncs as soon as Wi-Fi is up and then once every NTP_RESYNC_MS.
void startNtp();

// Call from loop(): copies a fresh NTP time into the DS3231.
void poll();

// False until the clock has been set by the RTC or NTP.
bool valid();
}  // namespace TimeSync
