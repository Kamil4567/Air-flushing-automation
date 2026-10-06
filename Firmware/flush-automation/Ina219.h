#ifndef INA219_H
#define INA219_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool ina219_bus_read_register(uint8_t address, uint8_t reg, uint16_t *value);
bool ina219_read_current_ma(float *current_ma);

#ifdef __cplusplus
}
#endif

#endif