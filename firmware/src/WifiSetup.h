#pragma once

// Wi-Fi provisioning. The network name and password are not in the firmware: a device with nothing saved
// opens its own "RTD-Logger-XXXX" network and serves a setup page; the credentials the user enters are kept
// in flash. Holding the BOOT button for WIFI_RESET_HOLD_MS erases them again, and so does losing the saved
// network for WIFI_FALLBACK_MS (the setup network reopens, the saved details are kept and still retried).
namespace WifiSetup {
// Loads the saved credentials and connects, or starts the setup network if there are none.
void begin();

// Call from loop(): serves the setup page, retries the connection and watches the reset button.
void poll();
}  // namespace WifiSetup
