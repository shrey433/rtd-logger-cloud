// Temporary SPI bus diagnostic for the 8 MAX31865 modules. Build with: pio run -e diag-n16r8 -t upload
// It prints, every few seconds, what each pin and each chip is doing, so a wiring fault can be located.
#include <Arduino.h>
#include <SPI.h>

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
  bus.end();
  delay(4000);
}
