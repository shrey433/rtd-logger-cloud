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

// Configuration register bits.
constexpr uint8_t kCfgBias = 0x80;
constexpr uint8_t kCfgOneShot = 0x20;
constexpr uint8_t kCfgThreeWire = 0x10;
constexpr uint8_t kCfgFaultClear = 0x02;
constexpr uint8_t kCfgFilter50Hz = 0x01;
constexpr uint8_t kBiasSettleMs = 12;  // the input filter needs about 10.5 ms to settle
constexpr int kConversionTimeoutMs = 200;  // a conversion takes about 65 ms; the one-shot bit clears when done

SPIClass bus(FSPI);
Adafruit_MAX31865* chip[kChannels];
bool wasMissing[kChannels];  // lets us log a change once, not every cycle
bool wasDead[kChannels];
bool wasStuck[kChannels];

float nominal(int ch) { return CH_IS_PT1000[ch] ? R_NOMINAL_PT1000 : R_NOMINAL_PT100; }
float reference(int ch) { return CH_IS_PT1000[ch] ? R_REF_PT1000 : R_REF_PT100; }

void regRead(int ch, uint8_t reg, uint8_t* out, int n) {
  bus.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE1));
  digitalWrite(PIN_CS[ch], LOW);
  bus.transfer(reg);
  for (int i = 0; i < n; i++) out[i] = bus.transfer(0x00);
  digitalWrite(PIN_CS[ch], HIGH);
  bus.endTransaction();
}

void regWrite(int ch, uint8_t reg, uint8_t value) {
  bus.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE1));
  digitalWrite(PIN_CS[ch], LOW);
  bus.transfer(reg | 0x80);
  bus.transfer(value);
  digitalWrite(PIN_CS[ch], HIGH);
  bus.endTransaction();
}

// True if the chip answers on SPI. Reads the fault threshold registers, which hold FF FF 00 00 on every
// MAX31865 (we never change them); a dead, unpowered or unwired chip reads all zeros, or all ones if the
// line floats high, so it can never match.
bool responds(int ch) {
  uint8_t t[4];
  regRead(ch, 0x03, t, 4);
  return t[0] == 0xFF && t[1] == 0xFF && t[2] == 0x00 && t[3] == 0x00;
}

struct Probe {
  bool alive;  // the module answers on SPI at all
  bool stuck;  // it answers but never finishes a conversion, so any result register is stale
  uint8_t cfgBefore;  // configuration register as found, before this conversion
  uint16_t raw;
  uint8_t fault;
  float temp;
  bool missing;  // no usable sensor: open leads, a reading beyond the sensor's range, or a short
};

// One conversion with the bias on, the same sequence the library uses, except that the fault register
// is read while the bias is still on and only then is the bias switched off. Reading it after the
// switch-off (as the library's readRTD() + readFault() do) catches a false over/under-voltage flag.
bool convert(int ch, uint16_t* raw, uint8_t* fault) {
  const uint8_t base = (CH_THREE_WIRE[ch] ? kCfgThreeWire : 0) | (MAINS_HZ == 50 ? kCfgFilter50Hz : 0);
  regWrite(ch, 0x00, kCfgBias | base);
  delay(kBiasSettleMs);
  regWrite(ch, 0x00, kCfgBias | kCfgOneShot | base);
  bool done = false;
  for (int waited = 0; waited < kConversionTimeoutMs; waited += 5) {
    delay(5);
    uint8_t cfg;
    regRead(ch, 0x00, &cfg, 1);
    if (!(cfg & kCfgOneShot)) {  // the chip clears the one-shot bit when the result is ready
      done = true;
      break;
    }
  }
  uint8_t r[8];
  regRead(ch, 0x00, r, 8);  // 0x00 config, 0x01..0x02 RTD result, 0x07 fault status
  *raw = (uint16_t)(((r[1] << 8) | r[2]) >> 1);
  *fault = r[7];
  regWrite(ch, 0x00, base | kCfgFaultClear);  // bias off, clear the fault status
  return done;
}

Probe probe(int ch) {
  Probe p = {};
  p.alive = responds(ch);
  if (!p.alive) {
    p.temp = NAN;
    return p;
  }
  regRead(ch, 0x00, &p.cfgBefore, 1);
  if (!convert(ch, &p.raw, &p.fault)) {
    p.stuck = true;
    p.temp = NAN;
    return p;
  }
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
  for (int ch = 0; ch < kChannels; ch++) {  // deselect every chip before talking to any of them
    pinMode(PIN_CS[ch], OUTPUT);
    digitalWrite(PIN_CS[ch], HIGH);
  }
  for (int ch = 0; ch < kChannels; ch++) {
    regWrite(ch, 0x00, 0x00);  // drop any leftover state: bias, auto-convert, fault detection
    delay(2);
    regWrite(ch, 0x00, kCfgFaultClear);
    chip[ch] = new Adafruit_MAX31865(PIN_CS[ch], &bus);
    chip[ch]->begin(CH_THREE_WIRE[ch] ? MAX31865_3WIRE : MAX31865_2WIRE);
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
    } else if (p.stuck) {
      c = NAN;  // never report a stale result register as a temperature
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
    if (p.stuck != wasStuck[ch]) {
      wasStuck[ch] = p.stuck;
      if (p.stuck) Serial.printf("ch%d: conversion did not complete, chip is stuck (power-cycle the modules), reporting null\n", ch + 1);
      else Serial.printf("ch%d: converting again\n", ch + 1);
    }
    if (p.alive && !p.stuck && p.missing != wasMissing[ch]) {
      wasMissing[ch] = p.missing;
      if (p.missing) Serial.printf("ch%d: no sensor detected, reporting 0.00\n", ch + 1);
      else Serial.printf("ch%d: sensor detected\n", ch + 1);
    }
    out.temp_c[ch] = c;
  }
  return good;
}

void selfTest() {
  Serial.println("sensor self-test (raw = 15-bit ADC count, R = raw / 32768 * R_REF):");
  for (int ch = 0; ch < kChannels; ch++) {
    Probe p = probe(ch);
    float r = p.raw / 32768.0f * reference(ch);
    const char* verdict = !p.alive ? "MODULE NOT RESPONDING -> null"
                          : p.stuck   ? "CONVERSION STUCK -> null"
                          : p.missing ? "no sensor -> 0.00"
                          : p.fault   ? "FAULT -> null"
                                      : "ok";
    Serial.printf("  ch%d %-6s %s cfg=0x%02X raw=%5u fault=0x%02X R=%8.2f ohm  T=%8.2f C  %s\n", ch + 1,
                  CH_IS_PT1000[ch] ? "PT1000" : "PT100", CH_THREE_WIRE[ch] ? "3w" : "2w", p.cfgBefore, p.raw, p.fault,
                  r, p.temp, verdict);
  }
}

}  // namespace Sensors
