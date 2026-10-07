#pragma once
#include "ReadingQueue.h"

namespace Sensors {
// Starts the shared SPI bus and puts all 8 MAX31865 chips in 3-wire mode.
void begin();

// One-shot conversion on every channel (about 0.6 s total). Faulted channels are set to NAN.
// Returns how many channels read cleanly.
int sample(Reading& out);
}  // namespace Sensors
