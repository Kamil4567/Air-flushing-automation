#include <Wire.h>

#include "ConfigStore.h"
#include "Ina219.h"
#include "Procedure.h"
#include "Ui.h"

extern "C" bool ina219_bus_read_register(
    uint8_t address,
    uint8_t reg,
    uint16_t *value) {
  Wire.beginTransmission(address);
  Wire.write(reg);

  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  if (Wire.requestFrom(address, static_cast<uint8_t>(2)) != 2) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }

  const uint8_t highByte = Wire.read();
  const uint8_t lowByte = Wire.read();
  *value = (static_cast<uint16_t>(highByte) << 8) | lowByte;
  return true;
}

void setup() {
  configStoreBegin();
  uiBegin();
  procedureBegin();
}

void loop() {
  processInput();
  updateScreen();
  procedureUpdate();
  updateScreen();
  configStoreService();
}