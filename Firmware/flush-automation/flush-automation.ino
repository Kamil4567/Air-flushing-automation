#include <Wire.h>

#include "ConfigStore.h"
#include "Ina219.h"
#include "Lps28dfw.h"
#include "Procedure.h"
#include "Ui.h"

// Odczytuje dwa bajty wskazanego rejestru przez I2C z powtórzonym START.
// Argumenty: adres urządzenia, numer rejestru i wskaźnik na wynik.
// Zwraca: powodzenie transakcji; przy sukcesie zapisuje wartość 16-bitową do *value.
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

// Zapisuje dwa bajty (MSB first) do wskazanego rejestru przez I2C.
// Argumenty: adres urządzenia, numer rejestru i wartość 16-bitowa.
// Zwraca: true, gdy urządzenie potwierdziło transakcję.
extern "C" bool ina219_bus_write_register(
    uint8_t address,
    uint8_t reg,
    uint16_t value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(static_cast<uint8_t>(value >> 8));
  Wire.write(static_cast<uint8_t>(value & 0xFF));
  return Wire.endTransmission() == 0;
}

// Sprawdza, czy urządzenie odpowiada na wskazanym adresie I2C.
// Argumenty: adres urządzenia. Zwraca: true, gdy odebrano ACK.
extern "C" bool ina219_bus_probe(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

// Zapisuje jeden bajt do rejestru czujnika ciśnienia LPS28DFW przez I2C.
// Argumenty: adres urządzenia, numer rejestru i wartość. Zwraca: true po potwierdzeniu (ACK).
extern "C" bool lps28dfw_bus_write_register(
    uint8_t address,
    uint8_t reg,
    uint8_t value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

// Odczytuje kolejne rejestry LPS28DFW (automatyczna inkrementacja adresu) z powtórzonym START.
// Argumenty: adres urządzenia, pierwszy rejestr, bufor i liczba bajtów (1-8).
// Zwraca: true, gdy odebrano wszystkie bajty.
extern "C" bool lps28dfw_bus_read_registers(
    uint8_t address,
    uint8_t reg,
    uint8_t *buffer,
    uint8_t length) {
  Wire.beginTransmission(address);
  Wire.write(reg);

  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  if (Wire.requestFrom(address, length) != length) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }

  for (uint8_t index = 0; index < length; ++index) {
    buffer[index] = Wire.read();
  }
  return true;
}

// Sprawdza, czy urządzenie odpowiada na wskazanym adresie I2C.
// Argumenty: adres urządzenia. Zwraca: true, gdy odebrano ACK.
extern "C" bool lps28dfw_bus_probe(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

// Czeka podaną liczbę milisekund podczas inicjalizacji czujnika ciśnienia.
// Argumenty: czas w ms. Zwraca: brak.
extern "C" void lps28dfw_bus_delay_ms(uint32_t milliseconds) {
  delay(milliseconds);
}

// Inicjalizuje pamięć ustawień, interfejs użytkownika i sterowanie procedurą.
// Argumenty: brak. Zwraca: brak; przygotowuje peryferia i stan firmware'u.
void setup() {
  configStoreBegin();
  uiBegin();
  ina219_begin();
  lps28dfw_begin();
  procedureBegin();
}

// Wykonuje jeden nieblokujący przebieg obsługi wejść, UI, procedury i zapisu.
// Argumenty: brak. Zwraca: brak; poszczególne moduły aktualizują swoje stany.
void loop() {
  processInput();
  updateScreen();
  procedureUpdate();
  updateScreen();
  configStoreService();
}