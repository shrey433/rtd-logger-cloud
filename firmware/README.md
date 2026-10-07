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
3. If Wi-Fi is down or the upload fails, push the row onto the PSRAM queue (40,000 rows, roughly 111 hours) and also
   append it to a LittleFS file on the flash `spiffs` partition (up to 36,000 rows, roughly 100 hours).
4. After a reset or power cut, `FlashBacklog::begin()` reloads every unsent row from flash into the queue. A row is
   released from flash only once the server has acknowledged it; if the device dies between the upload and that
   bookkeeping, the row is simply sent again and the server drops the duplicate.
5. A channel with a MAX31865 fault is sent as `null`. NTP re-syncs once a day and updates the DS3231.

Flash is only written while rows are being queued, so a healthy connection causes no flash wear. If flash cannot be
mounted the logger carries on with the RAM queue alone. Each upload also carries `backlog`, the number of rows still
waiting on the device, which the dashboard shows.

`partitions.csv` already reserves two app slots and `otadata`, so OTA can be added later without a USB reflash.
