#include "Uploader.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include <time.h>

#include "config.h"
#include "secrets.h"

namespace {
WiFiClientSecure tlsClient;
WiFiClient plainClient;
HTTPClient http;
bool tlsConfigured = false;

bool isHttps() { return strncmp(SERVER_URL, "https://", 8) == 0; }

void configureTls() {
  if (tlsConfigured) return;
  tlsConfigured = true;
#ifdef SERVER_CA_CERT
  tlsClient.setCACert(SERVER_CA_CERT);
#else
  tlsClient.setInsecure();
  Serial.println("WARNING: SERVER_CA_CERT not set, server certificate is not verified");
#endif
}

void isoTime(uint32_t epoch, char* out, size_t len) {
  time_t t = (time_t)epoch;
  struct tm tmv;
  gmtime_r(&t, &tmv);
  strftime(out, len, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

String buildBody(const Reading* rows, size_t count, size_t backlogRows) {
  JsonDocument doc;
  doc["device_id"] = DEVICE_ID;
  doc["fw_version"] = FW_VERSION;
  doc["backlog"] = (uint32_t)backlogRows;
  JsonArray readings = doc["readings"].to<JsonArray>();
  for (size_t i = 0; i < count; i++) {
    char ts[24];
    isoTime(rows[i].ts, ts, sizeof(ts));
    JsonObject r = readings.add<JsonObject>();
    r["ts"] = ts;
    JsonArray channels = r["channels"].to<JsonArray>();
    for (int ch = 0; ch < kChannels; ch++) {
      JsonObject c = channels.add<JsonObject>();
      c["ch"] = ch + 1;
      c["type"] = CH_IS_PT1000[ch] ? "PT1000" : "PT100";
      if (isnan(rows[i].temp_c[ch])) c["temp_c"] = nullptr;
      else c["temp_c"] = rows[i].temp_c[ch];
    }
  }
  String body;
  serializeJson(doc, body);
  return body;
}
}  // namespace

namespace Uploader {

Result post(const Reading* rows, size_t count, size_t backlogRows) {
  String body = buildBody(rows, count, backlogRows);

  http.setReuse(true);  // keep the TLS session open between 10 s cycles when the server allows it
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  bool began;
  if (isHttps()) {
    configureTls();
    began = http.begin(tlsClient, SERVER_URL);
  } else {
    began = http.begin(plainClient, SERVER_URL);
  }
  if (!began) return Result::Retry;

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " DEVICE_TOKEN);
  int code = http.POST(body);
  if (code < 0) Serial.printf("upload: %s\n", HTTPClient::errorToString(code).c_str());
  http.end();

  if (code >= 200 && code < 300) return Result::Ok;
  if (code == 400 || code == 413 || code == 422) {
    Serial.printf("upload: server rejected payload (%d), dropping rows\n", code);
    return Result::Drop;
  }
  if (code == 401 || code == 403) Serial.println("upload: token refused, check DEVICE_TOKEN");
  else if (code > 0) Serial.printf("upload: server answered %d\n", code);
  return Result::Retry;
}

}  // namespace Uploader
