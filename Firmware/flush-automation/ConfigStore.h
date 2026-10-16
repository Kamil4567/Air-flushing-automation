#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

// Inicjalizuje EEPROM i ładuje rekord aktualny, migruje rekord v1 lub stosuje defaults.
// Argumenty/wartość zwrotna: brak; ustawia globalną konfigurację i może zaplanować zapis.
void configStoreBegin();
// Zgłasza potrzebę odroczonego zapisu aktualnej konfiguracji.
// Argumenty/wartość zwrotna: brak; ustawia znacznik zapisu i termin debounce.
void configStoreRequestSave();
// Wykonuje oczekujący zapis po upływie terminu i potwierdza go w EEPROM.
// Argumenty/wartość zwrotna: brak; może wyczyścić znacznik oczekującego zapisu.
void configStoreService();

#endif