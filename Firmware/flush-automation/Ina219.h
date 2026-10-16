#ifndef INA219_H
#define INA219_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Odczytuje 16-bitowy rejestr czujnika przez magistralę I2C.
// Argumenty: adres urządzenia, adres rejestru i wskaźnik na wynik.
// Zwraca: true po udanym odczycie; wówczas zapisuje wynik do *value.
bool ina219_bus_read_register(uint8_t address, uint8_t reg, uint16_t *value);
// Odczytuje rejestr prądu INA219 i konwertuje signed wynik do mA.
// Argumenty: wskaźnik na wynik w mA. Zwraca: powodzenie odczytu;
// przy true zapisuje wartość do *current_ma.
bool ina219_read_current_ma(float *current_ma);

#ifdef __cplusplus
}
#endif

#endif