#include "Uploader.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <math.h>
#include <mqtt_client.h>
#include <time.h>

#include "TimeSync.h"
#include "config.h"
#include "secrets.h"

// PEM files from certs/, embedded by PlatformIO (board_build.embed_txtfiles) with a trailing NUL.
extern const char rootCa[] asm("_binary_certs_AmazonRootCA1_pem_start");
extern const char deviceCert[] asm("_binary_certs_device_cert_pem_start");
extern const char deviceKey[] asm("_binary_certs_device_private_key_start");

namespace {
const char* kTopic = "rtd/" DEVICE_ID "/telemetry";

esp_mqtt_client_handle_t client = nullptr;
bool started = false;
volatile bool up = false;
volatile int ackedId = 0;  // message id of the latest PUBACK; ids start at 1

void onMqttEvent(void*, esp_event_base_t, int32_t eventId, void* eventData) {
  auto* event = static_cast<esp_mqtt_event_handle_t>(eventData);
  switch (static_cast<esp_mqtt_event_id_t>(eventId)) {
    case MQTT_EVENT_CONNECTED:
      up = true;
      Serial.println("mqtt: connected");
      break;
    case MQTT_EVENT_DISCONNECTED:
      up = false;
      Serial.println("mqtt: disconnected, will retry");
      break;
    case MQTT_EVENT_PUBLISHED:
      ackedId = event->msg_id;
      break;
    case MQTT_EVENT_ERROR:
      if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
        Serial.printf("mqtt: broker refused the connection (code %d), check the IoT policy and DEVICE_ID\n",
                      (int)event->error_handle->connect_return_code);
      else if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
        Serial.printf("mqtt: transport error (tls 0x%x, errno %d)\n",
                      (unsigned)event->error_handle->esp_tls_last_esp_err, event->error_handle->esp_transport_sock_errno);
      break;
    default:
      break;
  }
}

void isoTime(uint32_t epoch, char* out, size_t len) {
  time_t t = (time_t)epoch;
  struct tm tmv;
  gmtime_r(&t, &tmv);
  strftime(out, len, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

String buildBody(const Reading& row, size_t backlogRows) {
  JsonDocument doc;
  char ts[24];
  isoTime(row.ts, ts, sizeof(ts));
  doc["fw_version"] = FW_VERSION;
  doc["backlog"] = (uint32_t)backlogRows;
  doc["ts"] = ts;
  JsonArray channels = doc["channels"].to<JsonArray>();
  for (int ch = 0; ch < kChannels; ch++) {
    JsonObject c = channels.add<JsonObject>();
    c["ch"] = ch + 1;
    c["type"] = CH_IS_PT1000[ch] ? "PT1000" : "PT100";
    if (isnan(row.temp_c[ch])) c["temp_c"] = nullptr;
    else c["temp_c"] = row.temp_c[ch];
  }
  String body;
  serializeJson(doc, body);
  return body;
}
}  // namespace

namespace Uploader {

void begin() {
  static String uri = String("mqtts://") + MQTT_HOST + ":" + String(MQTT_PORT);
  esp_mqtt_client_config_t cfg = {};
  cfg.broker.address.uri = uri.c_str();
  cfg.broker.verification.certificate = rootCa;
  cfg.credentials.client_id = DEVICE_ID;
  cfg.credentials.authentication.certificate = deviceCert;
  cfg.credentials.authentication.key = deviceKey;
  cfg.session.keepalive = 60;
  cfg.network.reconnect_timeout_ms = 5000;
  cfg.network.timeout_ms = 8000;
  client = esp_mqtt_client_init(&cfg);
  esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, onMqttEvent, nullptr);
}

void poll() {
  if (started || !client) return;
  if (WiFi.status() != WL_CONNECTED || !TimeSync::valid()) return;
  started = true;
  Serial.printf("mqtt: connecting to %s as %s\n", MQTT_HOST, DEVICE_ID);
  esp_mqtt_client_start(client);
}

bool ready() { return up; }

Result publish(const Reading& row, size_t backlogRows) {
  if (!up) return Result::Retry;
  String body = buildBody(row, backlogRows);
  ackedId = 0;
  int id = esp_mqtt_client_publish(client, kTopic, body.c_str(), body.length(), 1, 0);
  if (id <= 0) return Result::Retry;
  uint32_t start = millis();
  while (millis() - start < MQTT_ACK_TIMEOUT_MS) {
    if (ackedId == id) return Result::Ok;
    if (!up) break;
    delay(10);
  }
  Serial.println("mqtt: no acknowledgement, row stays queued");
  return Result::Retry;  // may still arrive later; the server drops the duplicate when it is re-sent
}

}  // namespace Uploader
