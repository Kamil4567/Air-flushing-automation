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
// Zapisuje 16-bitowy rejestr czujnika przez magistralę I2C.
// Argumenty: adres urządzenia, adres rejestru i wartość. Zwraca: true po potwierdzeniu (ACK).
bool ina219_bus_write_register(uint8_t address, uint8_t reg, uint16_t value);
// Sprawdza, czy urządzenie o danym adresie potwierdza zapytanie na magistrali I2C.
// Argumenty: adres urządzenia. Zwraca: true, gdy urządzenie odpowiada (ACK).
bool ina219_bus_probe(uint8_t address);
// Wyszukuje czujnik w zakresie 0x40-0x4F, konfiguruje go i weryfikuje zapis rejestrów.
// Argumenty: brak. Zwraca: true, gdy komunikacja działa i czujnik jest skonfigurowany.
bool ina219_begin(void);
// Zwraca adres skonfigurowanego czujnika albo 0, gdy komunikacja nie została nawiązana.
// Argumenty: brak.
uint8_t ina219_address(void);
// Odczytuje rejestr prądu INA219 i konwertuje signed wynik do mA.
// Argumenty: wskaźnik na wynik w mA. Zwraca: powodzenie odczytu;
// przy true zapisuje wartość do *current_ma.
bool ina219_read_current_ma(float *current_ma);

#ifdef __cplusplus
}
#endif

#endif