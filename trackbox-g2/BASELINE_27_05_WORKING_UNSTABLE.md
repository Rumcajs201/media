# TrackBox Gen2 — baza odniesienia 27.05 (działa, ale NIE jest stabilna)

**Status:** OSTATNIA POTWIERDZONA FUNKCJONALNIE WERSJA — NIETRWALE STABILNA / NIEUZASADNIONE UZNAWANIE ZA PRODUKCYJNĄ.

**Potwierdzenie użytkownika:** 2026-11-19, test po przywróceniu oryginalnego firmware 27.05.

## Ustalenia testowe

- Firmware **27.05** po ponownym wgraniu **łączy się z tachografem SE5000**.
- Na OLED widoczne są: **START DNIA, CZAS JAZDY, aktualny zegar, prędkość i przebieg**.
- Jest to ostatnia rozpoznana wersja, dla której użytkownik potwierdził jednocześnie połączenie i komplet tych funkcji.
- **NIE potwierdzono długotrwałej stabilności.** W poprzednich testach tego firmware zdarzały się rozłączenia i zawieszenie Bluetooth tachografu, wymagające odłączenia zasilania samochodu. Nie przypisywać tego mechanizmowi bez dalszych testów.
- W 27.05 wyświetlany START DNIA pochodzi z **F997 (koniec ostatniego odpoczynku dobowego)**. To przybliżenie godziny rozpoczęcia, a **nie osobny rekord startu pracy**.
- 26.22 także łączyła się z tachografem, ale nie miała potwierdzonego kompletu wyświetlanych funkcji. Problemy 27.06–27.09 nie zmieniają statusu 27.05.

## Zamrożony punkt odniesienia

- Gałąź: `baseline/trackbox-g2-27-05-working-not-stable`
- Pełny oryginalny snapshot źródła i binarki: commit `8d9c3cdb8c5037067f74d946b1815a052649bf69`
- Oryginalne źródło w snapshot: `trackbox-g2/TrackBox_Gen2/TrackBox_Gen2.ino`
- Git blob SHA źródła: `12e42dc53ae082d301b4574ff65786b2d7a3a764`
- Wersja wewnątrz programu: `2026.10.27.05-START-UI-F997-TEST`
- Oryginalna binarka: `trackbox-g2/firmware/latest.bin` w tym commicie
- Git blob SHA binarki: `33b6c304f121b67745822a7a0012a293f4123a49`
- SHA-256 BIN: `f331b308562e12e8c71dadfff3c76d9f7d0d0bbda8e169927112a69d1817e288`
- Rozmiar BIN: `709952` bajty
- Stała kopia binarki w `main`: `trackbox-g2/firmware/archive/TrackBox_G2_27.05_ORIGINAL.bin`
- Kopia wcześniejszej 26.22: `trackbox-g2/firmware/archive/TrackBox_G2_26.22_ORIGINAL.bin`

W dniu 2026-11-19 opublikowano **dokładnie tę samą binarkę 27.05** na zwykłym kanale OTA (bez rekompilacji). Jedynie zewnętrzny plik `version.txt` otrzymał znacznik `2026.11.19.27.05-ORIGINAL-OTA-TEST`, aby OTA wykryła zmianę. To **nie jest zmieniona wersja programu**.

## Zasady dalszej pracy

1. **Nie zmieniać, nie rebase’ować i nie usuwać** zamrożonej gałęzi bazowej ani jej artefaktów.
2. Każdą diagnostykę lub poprawkę rozwijać w osobnej gałęzi/wersji z wyraźnym oznaczeniem, bez nadpisywania oryginalnego 27.05.
3. Nie zmieniać pinów, interfejsu, odczytów i funkcji bez technicznej konieczności.
4. Priorytet: wykryć przyczynę rozłączeń BLE oraz sytuacji, w której sam SE5000 przestaje być widoczny dla telefonu.
5. Długotrwała praca, brak blokady tachografu i zachowanie przy Wi-Fi wymagają odrębnego potwierdzenia na urządzeniu. Nie określać 27.05 jako „stabilna”.
6. Kolejne testy na postoju, z ograniczeniem ryzyka kolejnego zawieszenia Bluetooth tachografu.

**Ta dokumentacja jest zapisem wyników i zabezpieczeniem referencji, a nie potwierdzeniem naprawy problemu.**
