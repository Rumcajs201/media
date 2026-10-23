# TrackBox G2 — prototyp 23.11: lokalny zegar

Status na 2026-10-23: **źródło zapisane, kompilacja i test fizyczny niepotwierdzone**. Nie używać zamiast sprawdzonej 23.10 bez testów.

## Założenia
- Normalny ekran: większa prędkość F902 (jedno miejsce po przecinku), niżej przebieg F912 (jedno miejsce po przecinku), na dole HH:MM.
- Ukryty dolny wiersz diagnostyczny F902 OK/licznik ramek. Diagnostyka BLE i błędy zachowane w kodzie.
- SH1106 128×64, ESP32-S3, wszystkie GPIO, UUID BLE, F902/F912 i istniejący licznik sekund >90 km/h pozostają bez zamierzonych zmian.
- Godzina: opcjonalny odczyt F90B TimeDate (8 bajtów) przez BLE, jednorazowo po uruchomieniu, później co godzinę. Czas z tachografu traktowany jako UTC, wyświetlany według reguły Norway CET/CEST (Europe/Oslo, automatyczna zmiana czasu).
- Pomiędzy synchronizacjami czas odmierza RTC systemowy ESP32, więc wymaga podtrzymania zasilania; po restarcie czeka na tachograf. Bez prawidłowej synchronizacji "--:--".
- Zegar ma dokładność minutową i nie wywołuje dodatkowego odrysowania co sekundę. Normalna prędkość dalej odświeża cały bufor OLED po nowych odczytach.
- Wariant testowy nie jest automatycznie publikowany jako `trackbox-g2/firmware/latest.bin`.

## Wymagane testy
1. Potwierdzić, że kompilacja Arduino CLI (esp32 core 3.3.7, NimBLE-Arduino 2.5.1, U8g2) się powiodła i dostarcza BIN.
2. Testować tylko na postoju: ekran i stabilność F902/F912 i Bluetooth; sprawdzić czy F90B jest akceptowany w tym konkretnym SE5000.
3. Porównać HH:MM z zegarem kabiny; test czasu CET/CEST, szczególnie po zmianie czasu 2026-10-25.
4. Zweryfikować zachowanie przy braku odpowiedzi F90B, po rozłączeniu BLE i po restarcie zasilania.
5. Potwierdzić zachowanie istniejącego trybu przekroczenia 90 km/h.

## Źródła
- EU 2016/799 Appendix 8, Table 40 (F90B 8 bytes): https://www.legislation.gov.uk/eur/2016/799/annex/1
- Espressif system time/timezone: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/system_time.html

## Wersje
- Nienaruszona główna gałąź: 2026.10.23.10-BLESCANPAIR.
- Prototyp: 2026.10.23.11-LOCALCLOCK-TEST (tylko w branch `prototype/trackbox-g2-clock-23-11`).
