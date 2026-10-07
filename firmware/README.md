# RTD logger firmware

ESP32-S3-WROOM-1-N8R2 reading 8 MAX31865 modules (7 PT100 + 1 PT1000, 3-wire) every 10 s and posting
them to the cloud server. Pin map and wiring are in `../docs/rtd-logger-spec.html` section 05.

## Build and flash

```bash
cp include/secrets.example.h include/secrets.h   # then fill in Wi-Fi, SERVER_URL, DEVICE_TOKEN
pio run -t upload
pio device monitor
```

PlatformIO needs Python 3.10 or newer. Host-side tests for the offline queue: `pio test -e native`
(needs a C++ compiler on the PATH).

## Check before first power-up

- Each module's solder jumpers are set for 3-wire.
- `R_REF_PT100` and `R_REF_PT1000` in `include/config.h` match the resistors actually fitted on your modules.
- `MAINS_HZ` is 50 or 60 for your supply.
- `SERVER_CA_CERT` is set in `secrets.h` if the server is on https, otherwise the server is not verified.

## How a cycle works

1. Wait for a valid clock (DS3231 or NTP), then read all 8 channels with one-shot conversions (about 0.6 s).
2. If Wi-Fi is up, POST the new row together with the oldest queued row, so a reconnect catches up one row per cycle.
3. If Wi-Fi is down or the upload fails, push the row onto the PSRAM queue (42,000 rows, roughly 116 hours).
4. A channel with a MAX31865 fault is sent as `null`. NTP re-syncs once a day and updates the DS3231.

`partitions.csv` already reserves two app slots and `otadata`, so OTA can be added later without a USB reflash.
