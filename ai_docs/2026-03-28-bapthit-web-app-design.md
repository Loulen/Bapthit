# Bapt'Hit Web App Design

## Context

Birthday party app for Baptiste. A punching bag generates scores automatically. The ESP32 hosts a web app that guests access by connecting to the ESP32's Wi-Fi network.

## Architecture

- **ESP32 in AP mode** — creates "BAPTHIT" Wi-Fi network, no internet dependency
- **HTTP server** (`esp_http_server`) — serves a single-page web app + JSON API
- **Score store** — in-memory array of `{timestamp, score}` structs
- **Mock data** on boot (~15 scores spread over a few hours), real sensor integration later

## Web App

Single HTML file with embedded CSS/JS. No external dependencies (no internet in AP mode).

### Scores Tab
- "Derniers Scores" — recent scores, most recent first (time + score)
- "Meilleurs Scores" — top scores, sorted by value descending

### Stats Tab
- Canvas-drawn line chart (no charting library)
- X axis: time, Y axis: score
- Individual scores as dots + smoothed trend line overlay

### Design
- Dark/boxing theme — dark background, bold neon/red accents, aggressive typography
- Mobile-first (party guests use phones)

## API

- `GET /` — serves the HTML page
- `GET /api/scores` — returns `{ "scores": [{"time": "20h01", "score": 999}, ...] }`

## Constraints

- No external JS/CSS libraries (AP mode = no internet)
- Single HTML file embedded in flash via `EMBED_TXTFILES`
- Anonymous scores only (no player names)
- App polls API every few seconds for fresh data
