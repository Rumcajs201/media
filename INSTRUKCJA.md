# Rumcajs Media Center — instalacja

## Ważne
Do repozytorium `media` wgrywamy wyłącznie zawartość tej paczki.
Nie są potrzebne: Firebase, package.json, TypeScript ani npm.

## Pliki wymagane w głównym katalogu
- index.html
- style.css
- app.js
- media.json
- manifest.webmanifest
- service-worker.js
- 404.html
- .nojekyll
- folder assets
- folder covers
- folder media

## GitHub Pages
Settings → Pages:
- Source: Deploy from a branch
- Branch: main
- Folder: / (root)

Adres:
https://rumcajs201.github.io/media/

## Dodanie muzyki
1. Wgraj MP3 do `media/music`.
2. Otwórz `media.json`.
3. Dodaj wpis:

{
  "type": "music",
  "title": "Tytuł piosenki",
  "subtitle": "Wykonawca",
  "description": "Krótki opis",
  "file": "./media/music/nazwa-pliku.mp3",
  "cover": "./covers/default-cover.svg",
  "allowDownload": true
}

## Typy materiałów
- music
- video
- photo
- document
- download

## Przykład filmu
{
  "type": "video",
  "title": "Film z imprezy",
  "subtitle": "2026",
  "description": "Krótki opis filmu",
  "file": "./media/video/film.mp4",
  "cover": "./covers/default-cover.svg",
  "allowDownload": true
}

## Przykład zdjęcia
{
  "type": "photo",
  "title": "Galeria — zdjęcie 1",
  "subtitle": "Urodziny",
  "description": "",
  "file": "./media/photos/zdjecie-1.jpg",
  "cover": "./media/photos/zdjecie-1.jpg",
  "allowDownload": true
}
