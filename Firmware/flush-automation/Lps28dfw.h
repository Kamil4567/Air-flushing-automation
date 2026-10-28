#ifndef LPS28DFW_H
#define LPS28DFW_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Zapisuje jeden bajt do rejestru czujnika ciśnienia przez magistralę I2C.
// Argumenty: adres urządzenia, adres rejestru i wartość. Zwraca: true po potwierdzeniu (ACK).
bool lps28dfw_bus_write_register(uint8_t address, uint8_t reg, uint8_t value);
// Odczytuje kolejne rejestry czujnika (z automatyczną inkrementacją adresu) z powtórzonym START.
// Argumenty: adres urządzenia, adres pierwszego rejestru, bufor i liczba bajtów.
// Zwraca: true, gdy odebrano wszystkie bajty.
bool lps28dfw_bus_read_registers(uint8_t address, uint8_t reg, uint8_t *buffer,
                                 uint8_t length);
// Sprawdza, czy urządzenie odpowiada na wskazanym adresie I2C.
// Argumenty: adres urządzenia. Zwraca: true, gdy odebrano ACK.
bool lps28dfw_bus_probe(uint8_t address);
// Czeka podaną liczbę milisekund (używane tylko podczas inicjalizacji czujnika).
// Argumenty: czas w ms. Zwraca: brak.
void lps28dfw_bus_delay_ms(uint32_t milliseconds);

// Wyszukuje LPS28DFW pod adresem 0x5C lub 0x5D (SA0), sprawdza WHO_AM_I i konfiguruje
// pomiar ciągły (25 Hz, uśrednianie 16, zakres 1260 hPa).
// Argumenty: brak. Zwraca: true, gdy czujnik odpowiada i został skonfigurowany.
bool lps28dfw_begin(void);
// Zwraca adres skonfigurowanego czujnika albo 0, gdy komunikacja nie została nawiązana.
// Argumenty: brak.
uint8_t lps28dfw_address(void);
// Odczytuje ciśnienie bezwzględne w hPa. Przy błędzie komunikacji czujnik jest
// inicjalizowany ponownie przy następnym wywołaniu.
// Argumenty: wskaźnik na wynik w hPa. Zwraca: true, gdy odczyt jest poprawny i mieści się
// w zakresie pracy czujnika (nieprzygotowana próbka jest odrzucana); wynik zapisuje do *hpa.
bool lps28dfw_read_pressure_hpa(float *hpa);

#ifdef __cplusplus
}
#endif

#endif
