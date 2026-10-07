#pragma once
#include "ReadingQueue.h"

namespace Sensors {
// Starts the shared SPI bus and puts all 8 MAX31865 chips in 3-wire mode.
void begin();

// One-shot conversion on every channel (about 0.6 s total). A channel with no sensor connected
// (open leads, or a reading beyond what a PT100/PT1000 can give) reports 0.0; any other fault is NAN.
// Returns how many channels have a sensor and read cleanly.
int sample(Reading& out);

// Prints raw ADC value, fault bits, resistance and temperature of every channel once, so a wiring or
// module problem is visible on the serial monitor straight after boot.
void selfTest();
}  // namespace Sensors
