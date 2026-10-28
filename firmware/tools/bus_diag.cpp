// Temporary SPI bus diagnostic for the 8 MAX31865 modules. Build with: pio run -e diag-n16r8 -t upload
// It prints, every few seconds, what each pin and each chip is doing, so a wiring fault can be located.
#include <Arduino.h>
#include <SPI.h>
#include <driver/gpio.h>

#include "../src/ReadingQueue.h"  // kChannels
#include "config.h"

SPIClass bus(FSPI);

static int highCount(int pin, int mode) {
  pinMode(pin, mode);
  delay(3);
  int n = 0;
  for (int i = 0; i < 20; i++) n += digitalRead(pin);
  return n;
}

// Drive a pin high then low and report whether it follows (it will not if shorted to a rail or another pin).
static const char* driveTest(int pin) {
  pinMode(pin, OUTPUT);
  digitalWrite(pin, HIGH);
  delay(2);
  int hi = digitalRead(pin);
  digitalWrite(pin, LOW);
  delay(2);
  int lo = digitalRead(pin);
  digitalWrite(pin, HIGH);
  if (hi && !lo) return "ok";
  if (!hi && !lo) return "STUCK LOW (short to GND?)";
  if (hi && lo) return "STUCK HIGH (short to 3V3?)";
  return "inverted?";
}

static void readRegs(int ch, uint8_t spiMode, uint32_t hz, uint8_t* out, int n) {
  bus.beginTransaction(SPISettings(hz, MSBFIRST, spiMode));
  digitalWrite(PIN_CS[ch], LOW);
  bus.transfer(0x00);
  for (int i = 0; i < n; i++) out[i] = bus.transfer(0x00);
  digitalWrite(PIN_CS[ch], HIGH);
  bus.endTransaction();
}

static void writeReg(int ch, uint8_t reg, uint8_t value, uint8_t spiMode, uint32_t hz) {
  bus.beginTransaction(SPISettings(hz, MSBFIRST, spiMode));
  digitalWrite(PIN_CS[ch], LOW);
  bus.transfer(reg | 0x80);
  bus.transfer(value);
  digitalWrite(PIN_CS[ch], HIGH);
  bus.endTransaction();
}

// Reads the configuration register with a weak pull on MISO: +1 pull-up, -1 pull-down, 0 none.
// A chip that drives the line overrides the pull; a chip that is absent or unpowered lets it win.
static uint8_t readCfgBiased(int ch, int bias) {
  gpio_pullup_dis((gpio_num_t)PIN_MISO);
  gpio_pulldown_dis((gpio_num_t)PIN_MISO);
  if (bias > 0) gpio_pullup_en((gpio_num_t)PIN_MISO);
  if (bias < 0) gpio_pulldown_en((gpio_num_t)PIN_MISO);
  uint8_t cfg;
  readRegs(ch, SPI_MODE1, 100000, &cfg, 1);
  gpio_pullup_dis((gpio_num_t)PIN_MISO);
  gpio_pulldown_dis((gpio_num_t)PIN_MISO);
  return cfg;
}

// One conversion with the chip's 3-wire bit set or cleared. Returns the 15-bit ADC count, or -1 if the
// chip flagged a fault in the result register.
static int convertOnce(int ch, bool threeWire, int settleMs = 12, uint8_t* faultOut = nullptr) {
  uint8_t base = (threeWire ? 0x10 : 0x00) | (MAINS_HZ == 50 ? 0x01 : 0x00);
  writeReg(ch, 0x00, 0x80 | base, SPI_MODE1, 1000000);        // bias on
  delay(settleMs);
  writeReg(ch, 0x00, 0x80 | 0x20 | base, SPI_MODE1, 1000000);  // one-shot
  delay(70);
  uint8_t r[2];
  bus.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE1));
  digitalWrite(PIN_CS[ch], LOW);
  bus.transfer(0x01);
  r[0] = bus.transfer(0x00);
  r[1] = bus.transfer(0x00);
  digitalWrite(PIN_CS[ch], HIGH);
  bus.endTransaction();
  uint8_t fr[8];
  readRegs(ch, SPI_MODE1, 1000000, fr, 8);                     // fr[7] = fault status register
  if (faultOut) *faultOut = fr[7];
  writeReg(ch, 0x00, base | 0x02, SPI_MODE1, 1000000);        // bias off, clear fault status
  if (r[1] & 1) return -1;
  return ((r[0] << 8) | r[1]) >> 1;
}

static float pt100Temp(float ohm) {  // IEC 60751, valid above 0 C
  const float A = 3.9083e-3f, B = -5.775e-7f;
  if (ohm < 100.0f) return -99.0f;
  return (-A + sqrtf(A * A - 4 * B * (1 - ohm / 100.0f))) / (2 * B);
}

static void printRow(const char* label, const uint8_t* b, int n) {
  Serial.printf("      %-26s", label);
  for (int i = 0; i < n; i++) Serial.printf(" %02X", b[i]);
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\nMAX31865 bus diagnostic");
  for (int ch = 0; ch < kChannels; ch++) {
    pinMode(PIN_CS[ch], OUTPUT);
    digitalWrite(PIN_CS[ch], HIGH);
  }
}

void loop() {
  Serial.println("\n==== pass ====");
  // 1. every output pin must be able to swing, otherwise it is shorted
  Serial.printf("SCK  GPIO%-2d drive: %s\n", PIN_SCK, driveTest(PIN_SCK));
  Serial.printf("MOSI GPIO%-2d drive: %s\n", PIN_MOSI, driveTest(PIN_MOSI));
  for (int ch = 0; ch < kChannels; ch++)
    Serial.printf("CS%d  GPIO%-2d drive: %s\n", ch + 1, PIN_CS[ch], driveTest(PIN_CS[ch]));
  for (int ch = 0; ch < kChannels; ch++) digitalWrite(PIN_CS[ch], HIGH);

  // 2. MISO with every chip deselected: a healthy shared line floats, so the pull decides what we read
  int up = highCount(PIN_MISO, INPUT_PULLUP);
  int down = highCount(PIN_MISO, INPUT_PULLDOWN);
  Serial.printf("MISO GPIO%d, all CS high: reads high %d/20 with pull-up, %d/20 with pull-down -> %s\n", PIN_MISO, up, down,
                (up >= 18 && down <= 2) ? "floating (normal)"
                : (up <= 2)             ? "HELD LOW by something (short to GND, or an unpowered module loading it)"
                : (down >= 18)          ? "HELD HIGH by something (short to 3V3)"
                                        : "unstable");

  // 3. talk to each chip in both SPI modes at a slow and a normal clock
  bus.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);
  Serial.println("registers 0x00..0x07 after a plain read (a powered, working chip at default shows 00 00 00 FF FF 00 00 00):");
  for (int ch = 0; ch < kChannels; ch++) {
    Serial.printf("  ch%d (CS GPIO%d)\n", ch + 1, PIN_CS[ch]);
    uint8_t b[8];
    readRegs(ch, SPI_MODE1, 100000, b, 8);  printRow("mode 1, 100 kHz", b, 8);
    readRegs(ch, SPI_MODE3, 100000, b, 8);  printRow("mode 3, 100 kHz", b, 8);
    readRegs(ch, SPI_MODE1, 1000000, b, 8); printRow("mode 1, 1 MHz", b, 8);
    writeReg(ch, 0x00, 0x11, SPI_MODE1, 100000);
    uint8_t cfg;
    readRegs(ch, SPI_MODE1, 100000, &cfg, 1);
    Serial.printf("      wrote config 0x11, read back 0x%02X -> %s\n", cfg, cfg == 0x11 ? "chip WORKS" : "no answer");
  }
  Serial.println("MISO bias test, config register read with a weak pull-up / pull-down on MISO:");
  Serial.println("  a chip that answers gives the same value both ways; one that is not driving MISO follows the pull (FF up, 00 down)");
  for (int ch = 0; ch < kChannels; ch++) {
    uint8_t none = readCfgBiased(ch, 0), up = readCfgBiased(ch, +1), down = readCfgBiased(ch, -1);
    const char* verdict = (up == down) ? ((up == 0x00 || up == 0xFF) ? "line held steady at one level (chip driving a constant, or MISO stuck)" : "chip ANSWERS")
                                       : "MISO FOLLOWS THE PULL: this chip is not driving MISO (unpowered, CS or SDO not connected)";
    Serial.printf("  ch%d (CS GPIO%-2d): none %02X, pull-up %02X, pull-down %02X -> %s\n", ch + 1, PIN_CS[ch], none, up, down, verdict);
  }
  Serial.println("wire-mode test: 5 readings per setting, as PT100 on a 430 ohm reference (ignore empty channels):");
  for (int ch = 0; ch < kChannels; ch++) {
    float sum[2] = {0, 0};
    int ok[2] = {0, 0};
    for (int m = 0; m < 2; m++) {
      for (int i = 0; i < 5; i++) {
        int raw = convertOnce(ch, m == 1);
        if (raw >= 0 && raw < 32767) { sum[m] += raw; ok[m]++; }
      }
    }
    Serial.printf("  ch%d:", ch + 1);
    for (int m = 0; m < 2; m++) {
      if (!ok[m]) { Serial.printf("  %s: no valid reading     ", m ? "3-wire bit" : "2/4-wire bit"); continue; }
      float raw = sum[m] / ok[m], ohm = raw / 32768.0f * 430.0f;
      Serial.printf("  %s: raw %6.0f = %6.2f ohm = %6.1f C", m ? "3-wire bit" : "2/4-wire bit", raw, ohm, pt100Temp(ohm));
    }
    Serial.println();
  }
  Serial.println("settle-time test: 12 conversions each on channels that hold a sensor (faults = conversions that flagged one):");
  for (int ch = 0; ch < kChannels; ch++) {
    int first = convertOnce(ch, true, 20);
    if (first < 3000 || first > 30000) continue;  // empty or silent channel
    struct { bool three; int settle; } combos[] = {{true, 10}, {true, 12}, {true, 20}, {true, 40}, {false, 20}};
    for (auto& c : combos) {
      int bad = 0, good = 0;
      float sum = 0;
      uint8_t lastFault = 0;
      for (int i = 0; i < 12; i++) {
        uint8_t f = 0;
        int raw = convertOnce(ch, c.three, c.settle, &f);
        if (raw < 0 || (f & 0xFC)) { bad++; lastFault = f; } else { good++; sum += raw; }
      }
      Serial.printf("  ch%d %s bit, settle %2d ms: faults %2d/12 (last 0x%02X), mean raw %6.0f = %6.2f ohm\n", ch + 1,
                    c.three ? "3-wire" : "2/4-w ", c.settle, bad, lastFault, good ? sum / good : 0.0f,
                    good ? sum / good / 32768.0f * 430.0f : 0.0f);
    }
  }
  bus.end();
  delay(4000);
}
