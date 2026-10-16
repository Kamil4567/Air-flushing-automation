#ifndef PROCEDURE_H
#define PROCEDURE_H

// Inicjalizuje GPIO, zapisany próg prądu i sygnał startowy.
// Argumenty/wartość zwrotna: brak; przygotowuje stan sterowania procedurą.
void procedureBegin();
// Rozpoczyna procedurę, jeśli nie jest już aktywna.
// Argumenty/wartość zwrotna: brak; inicjuje automat, timer i ekran statusu.
void procedureStart();
// Zatrzymuje procedurę, wyłącza wyjścia i wraca do ekranu głównego.
// Argumenty/wartość zwrotna: brak; dezaktywuje automat i sygnalizuje zatrzymanie.
void procedureStop();
// Ustawia ręczne wyjścia dla napełniania/opróżniania; sprzeczne żądania wyłączają wyjścia.
// Argumenty: flagi napełniania i opróżniania. Wartość zwrotna: brak; zmienia GPIO i stan maszyny.
void procedureSetManualOutputs(bool filling, bool emptying);
// Aktualizuje dźwięk i wykonuje krok aktywnego automatu procedury.
// Argumenty/wartość zwrotna: brak; może zmieniać etapy, wyjścia i ekran statusu.
void procedureUpdate();
// Odtwarza krótki sygnał obrotu enkodera.
// Argumenty/wartość zwrotna: brak; zastępuje aktualną sekwencję brzęczyka.
void procedureEncoderTurnBeep();
// Odtwarza sygnał naciśnięcia enkodera.
// Argumenty/wartość zwrotna: brak; zastępuje aktualną sekwencję brzęczyka.
void procedureEncoderPressBeep();
// Odtwarza sygnał błędu wejścia poza zakresem.
// Argumenty/wartość zwrotna: brak; zastępuje aktualną sekwencję brzęczyka.
void procedureEncoderErrorBeep();
// Zleca reset programowy po odtworzeniu sygnału potwierdzającego.
// Argumenty/wartość zwrotna: brak; ustawia flagę resetu w sekwencji dźwiękowej.
void procedureRequestSoftwareReset();

#endif