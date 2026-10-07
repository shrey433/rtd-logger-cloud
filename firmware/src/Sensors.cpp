#include "Sensors.h"

#include <Adafruit_MAX31865.h>
#include <SPI.h>
#include <math.h>

#include "config.h"

static_assert(sizeof(PIN_CS) / sizeof(PIN_CS[0]) == kChannels, "one chip-select per channel");

namespace {
constexpr float kMinValidC = -200.0f;
constexpr float kMaxValidC = 850.0f;

SPIClass bus(FSPI);
Adafruit_MAX31865* chip[kChannels];

float nominal(int ch) { return CH_IS_PT1000[ch] ? R_NOMINAL_PT1000 : R_NOMINAL_PT100; }
float reference(int ch) { return CH_IS_PT1000[ch] ? R_REF_PT1000 : R_REF_PT100; }
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
    // readRTD() runs a one-shot conversion with the bias current on only for its duration.
    uint16_t raw = chip[ch]->readRTD();
    uint8_t fault = chip[ch]->readFault();
    float c = NAN;
    if (fault) {
      Serial.printf("ch%d fault 0x%02X\n", ch + 1, fault);
      chip[ch]->clearFault();
    } else {
      float t = chip[ch]->calculateTemperature(raw, nominal(ch), reference(ch));
      if (isfinite(t) && t >= kMinValidC && t <= kMaxValidC) {
        c = roundf(t * 100.0f) / 100.0f;
        good++;
      }
    }
    out.temp_c[ch] = c;
  }
  return good;
}

}  // namespace Sensors
