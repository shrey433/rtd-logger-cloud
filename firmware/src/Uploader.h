#pragma once
#include <stddef.h>

#include "ReadingQueue.h"

namespace Uploader {
enum class Result {
  Ok,     // the broker acknowledged the message (QoS 1 PUBACK); safe to forget the row
  Retry,  // not connected, timed out, or refused: keep the row and try again
};

// Configures the MQTT client (certificate, client id, broker). Call once in setup().
void begin();

// Call from loop(): starts the client once Wi-Fi and a valid clock exist (TLS checks certificate
// dates). The client then keeps reconnecting by itself.
void poll();

// True while the MQTT session to the broker is up.
bool ready();

// Publishes one reading to rtd/<DEVICE_ID>/telemetry and waits for the broker's acknowledgement.
// `backlogRows` is how many rows will still be waiting on the device once this one is delivered.
Result publish(const Reading& row, size_t backlogRows);
}  // namespace Uploader
