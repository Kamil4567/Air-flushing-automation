#include "Ina219.h"

#define INA219_ADDRESS 0x40
#define INA219_CURRENT_REGISTER 0x04
#define INA219_CURRENT_LSB_MA 0.1f

bool ina219_read_current_ma(float *current_ma) {
  uint16_t raw_value;

  if (!ina219_bus_read_register(
          INA219_ADDRESS,
          INA219_CURRENT_REGISTER,
          &raw_value)) {
    return false;
  }

  int32_t signed_value = raw_value;
  if ((raw_value & 0x8000u) != 0) {
    signed_value -= 0x10000;
  }

  *current_ma = (float)signed_value * INA219_CURRENT_LSB_MA;
  return true;
}