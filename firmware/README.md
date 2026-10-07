# RTD logger firmware

ESP32-S3-WROOM-1-N8R2 reading 8 MAX31865 modules (7 PT100 + 1 PT1000, 3-wire) every 10 s and publishing
them over MQTT to AWS IoT Core. Pin map and wiring are in `../docs/rtd-logger-spec.html` section 05.

## Build and flash

```bash
cp include/secrets.example.h include/secrets.h   # then fill in WIFI_SSID, WIFI_PASSWORD and MQTT_HOST
# copy the device certificate files into certs/ (commands in certs/README.md)
pio run -t upload
pio device monitor
```

PlatformIO needs Python 3.10 or newer. Host-side tests for the offline queue: `pio test -e native`
(needs a C++ compiler on the PATH).

A healthy start prints `mqtt: connected` within a few seconds of Wi-Fi and the clock being ready. If it prints
`broker refused the connection`, `DEVICE_ID` in `include/config.h` does not match the IoT thing name of the
certificate. A TLS error with a `tls` code usually means the clock was wrong when it connected or the root CA
file is not `AmazonRootCA1.pem`.

## Check before first power-up

- Each module's solder jumpers are set for 3-wire.
- `R_REF_PT100` and `R_REF_PT1000` in `include/config.h` match the resistors actually fitted on your modules.
- `MAINS_HZ` is 50 or 60 for your supply.
- `certs/` holds the three files and `DEVICE_ID` equals the certificate's thing name (`rtd-logger-01`).

## How a cycle works

1. Wait for a valid clock (DS3231 or NTP), then read all 8 channels with one-shot conversions (about 0.6 s).
2. If the MQTT session is up, publish the oldest queued row (if any) and then the new row, each as its own QoS 1
   message on `rtd/<DEVICE_ID>/telemetry`. A row counts as delivered only when the broker's PUBACK arrives, so a
   reconnect catches up one row per cycle.
3. If MQTT is down or a publish is not acknowledged within 4 s, push the row onto the PSRAM queue (40,000 rows,
   roughly 111 hours) and also append it to a LittleFS file on the flash `spiffs` partition (up to 36,000 rows,
   roughly 100 hours).
4. After a reset or power cut, `FlashBacklog::begin()` reloads every unsent row from flash into the queue. A row is
   released from flash only once the broker has acknowledged it; if the device dies between the acknowledgement and
   that bookkeeping, the row is simply sent again and the server drops the duplicate.
5. A channel with a MAX31865 fault is sent as `null`. NTP re-syncs once a day and updates the DS3231.

Flash is only written while rows are being queued, so a healthy connection causes no flash wear. If flash cannot be
mounted the logger carries on with the RAM queue alone. Each message also carries `backlog`, the number of rows still
waiting on the device, which the dashboard shows.

`partitions.csv` already reserves two app slots and `otadata`, so OTA can be added later without a USB reflash.
