#include "Sensors.h"

#include <Adafruit_MAX31865.h>
#include <SPI.h>
#include <math.h>

#include "config.h"

static_assert(sizeof(PIN_CS) / sizeof(PIN_CS[0]) == kChannels, "one chip-select per channel");

namespace {
constexpr float kMinValidC = -200.0f;  // the range a PT100/PT1000 can actually produce
constexpr float kMaxValidC = 850.0f;

// MAX31865 fault bits that mean the sensor leads are open: REFIN- above/below 0.85 Vbias (D5/D4)
// and RTDIN- below 0.85 Vbias (D3). An unplugged probe sets these and drives the reading to full scale.
constexpr uint8_t kLeadFaultMask = 0x38;

SPIClass bus(FSPI);
Adafruit_MAX31865* chip[kChannels];
bool wasMissing[kChannels];  // lets us log a change once, not every cycle
bool wasDead[kChannels];

float nominal(int ch) { return CH_IS_PT1000[ch] ? R_NOMINAL_PT1000 : R_NOMINAL_PT100; }
float reference(int ch) { return CH_IS_PT1000[ch] ? R_REF_PT1000 : R_REF_PT100; }

// True if the chip answers on SPI. Reads its configuration register, which always has the 3-wire bit
// set once begin() has run, so it is never 0x00; a dead, unpowered or unwired chip reads all zeros
// (or all ones if the line floats high).
bool responds(int ch) {
  bus.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE1));
  digitalWrite(PIN_CS[ch], LOW);
  bus.transfer(0x00);  // address 0x00 = configuration, read
  uint8_t cfg = bus.transfer(0x00);
  digitalWrite(PIN_CS[ch], HIGH);
  bus.endTransaction();
  return cfg != 0x00 && cfg != 0xFF;
}

struct Probe {
  bool alive;  // the module answers on SPI at all
  uint16_t raw;
  uint8_t fault;
  float temp;
  bool missing;  // no usable sensor: open leads, a reading beyond the sensor's range, or a short
};

Probe probe(int ch) {
  Probe p = {};
  p.alive = responds(ch);
  if (!p.alive) {
    p.temp = NAN;
    return p;
  }
  p.raw = chip[ch]->readRTD();  // one-shot conversion, bias current on only while it runs
  p.fault = chip[ch]->readFault();
  p.temp = chip[ch]->calculateTemperature(p.raw, nominal(ch), reference(ch));
  bool inRange = isfinite(p.temp) && p.temp >= kMinValidC && p.temp <= kMaxValidC;
  p.missing = !inRange || (p.fault & kLeadFaultMask);
  return p;
}
}  // namespace

namespace Sensors {

void begin() {
  // The library's own SPI.begin() is a no-op once the bus is started, so pin it to our pins first.
  bus.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);
  for (int ch = 0; ch < kChannels; ch++) {
    chip[ch] = new Adafruit_MAX31865(PIN_CS[ch], &bus);
    chip[ch]->begin(MAX31865_3WIRE);
    chip[ch]->enable50Hz(MAINS_HZ == 50);
    chip[ch]->clearFault();
  }
}

int sample(Reading& out) {
  int good = 0;
  for (int ch = 0; ch < kChannels; ch++) {
    Probe p = probe(ch);
    float c;
    if (!p.alive) {
      c = NAN;  // a wiring or power problem, not an empty channel: stays visible as null
    } else if (p.missing) {
      c = 0.0f;  // nothing connected: report 0.00 as agreed
    } else if (p.fault) {
      c = NAN;  // a real fault on a connected sensor stays visible as null
      Serial.printf("ch%d fault 0x%02X\n", ch + 1, p.fault);
    } else {
      c = roundf(p.temp * 100.0f) / 100.0f;
      good++;
    }
    if (!p.alive != wasDead[ch]) {
      wasDead[ch] = !p.alive;
      if (!p.alive) Serial.printf("ch%d: module not responding on SPI (check SCK/MOSI/MISO, 3V3, GND, CS), reporting null\n", ch + 1);
      else Serial.printf("ch%d: module responding again\n", ch + 1);
    }
    if (p.alive && p.missing != wasMissing[ch]) {
      wasMissing[ch] = p.missing;
      if (p.missing) Serial.printf("ch%d: no sensor detected, reporting 0.00\n", ch + 1);
      else Serial.printf("ch%d: sensor detected\n", ch + 1);
    }
    if (p.alive && p.fault) chip[ch]->clearFault();
    out.temp_c[ch] = c;
  }
  return good;
}

void selfTest() {
  Serial.println("sensor self-test (raw = 15-bit ADC count, R = raw / 32768 * R_REF):");
  for (int ch = 0; ch < kChannels; ch++) {
    chip[ch]->clearFault();
    Probe p = probe(ch);
    float r = p.raw / 32768.0f * reference(ch);
    const char* verdict = !p.alive ? "MODULE NOT RESPONDING -> null"
                          : p.missing ? "no sensor -> 0.00"
                          : p.fault   ? "FAULT -> null"
                                      : "ok";
    Serial.printf("  ch%d %-6s raw=%5u fault=0x%02X R=%8.2f ohm  T=%8.2f C  %s\n", ch + 1,
                  CH_IS_PT1000[ch] ? "PT1000" : "PT100", p.raw, p.fault, r, p.temp, verdict);
    if (p.alive) chip[ch]->clearFault();
  }
}

}  // namespace Sensors
