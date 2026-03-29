# Score Claiming & Laptop Backend Design

## Problem

Users want to claim scores on the leaderboard with their name and a photo taken from their phone. Photos are too large for ESP32 storage. Scores should persist across reboots when possible, but the system must work standalone.

## Architecture

Three-tier degradation:

| Feature | No laptop | Laptop connected |
|---|---|---|
| Scores | Yes (volatile) | Yes (persisted) |
| Name claiming | Yes (volatile) | Yes (persisted) |
| Photos | No | Yes |
| Survives reboot | No | Yes |

The ESP32 remains the network owner and primary UI server. The laptop is an optional enhancement that adds persistence and photo storage.

## Score Data Model

```c
typedef struct {
    int id;              // unique incrementing ID
    int hour;
    int minute;
    int score;
    char name[32];       // empty = unclaimed
    bool has_photo;      // photo stored on laptop
} score_entry_t;
```

JSON response from `GET /api/scores`:

```json
{
  "scores": [
    {"id": 1, "time": "14h32", "score": 820, "name": "Lucas", "has_photo": true},
    {"id": 2, "time": "14h35", "score": 650, "name": "", "has_photo": false}
  ],
  "laptop_ip": "192.168.4.2"
}
```

- `name: ""` means unclaimed, UI shows "???" with tap-to-claim
- `laptop_ip` is null/absent when no laptop is registered

## ESP32 API Changes

### New endpoints

| Endpoint | Method | Purpose |
|---|---|---|
| `/api/scores/<id>/claim` | POST | Claim a score with `{"name": "Lucas"}`. Returns 404 (not found) or 409 (already claimed). |
| `/api/backend/register` | POST | Laptop registers itself with `{"ip": "192.168.4.2"}` |
| `/api/scores/<id>/photo_ok` | POST | Laptop confirms photo stored, sets `has_photo = true` |

### Modified endpoints

| Endpoint | Change |
|---|---|
| `GET /api/scores` | Include `id`, `name`, `has_photo`, `laptop_ip` in response |

### Background forwarding

When the laptop is registered, the ESP32 forwards events to it:
- New scores: `POST http://<laptop_ip>/api/scores` with score data
- Claims: `POST http://<laptop_ip>/api/scores/<id>/claim` with name

### Boot recovery

When a laptop registers, the ESP32 fetches `GET http://<laptop_ip>/api/scores/history` and populates its in-memory array with persisted scores and claims.

## Laptop Server

Lightweight Python server (Flask or FastAPI) with SQLite database.

### Endpoints

| Endpoint | Method | Purpose |
|---|---|---|
| `/api/scores` | POST | Receive new scores from ESP32 |
| `/api/scores/<id>/claim` | POST | Receive claim forwarded from ESP32 |
| `/api/photos/<id>` | POST | Phone uploads photo directly (multipart) |
| `/api/photos/<id>` | GET | Phone fetches photo thumbnail for display |
| `/api/scores/history` | GET | ESP32 pulls all persisted data on boot/reconnect |

### Storage

- Scores + claims in SQLite
- Photos on disk: `photos/<score_id>.jpg`

### Discovery

The laptop registers itself on startup by POSTing to `http://192.168.4.1/api/backend/register` with its IP. The ESP32 stores this IP in a global variable (null = no laptop).

## Photo Flow

1. User taps an unclaimed score ("???") on the ESP32 web UI
2. Modal opens with a name input field + optional "Take Photo" button (visible only when laptop is registered)
3. User types name and hits "Save" -> `POST /api/scores/<id>/claim` to ESP32
4. If laptop is available, phone camera opens via `<input type="file" accept="image/*" capture="environment">`
5. Phone uploads photo directly to laptop: `POST http://<laptop_ip>/api/photos/<id>`
6. Laptop confirms to ESP32: `POST /api/scores/<id>/photo_ok`
7. UI displays thumbnail from `http://<laptop_ip>/api/photos/<id>` for claimed scores with photos

## Web UI Changes

### Leaderboard

- Unclaimed scores display "???" and are tappable
- Claimed scores show name + photo thumbnail (if available)
- Claim modal: name text input, photo button (conditional on laptop), save/cancel

### Settings

- New "Backend" section showing connection status:
  - No laptop: "Backend: Not connected" (greyed out)
  - Laptop present: "Backend: Connected (192.168.4.x)" with green indicator

## What Does NOT Change

- PunchMeter component (sensor logic, 7-segment display)
- WiFi AP configuration (SSID, channel, DHCP)
- DNS captive portal
- OTA update system
- Core 0 / Core 1 task split

## Data Flow

```
Claim with photo (laptop present):

  Phone              ESP32              Laptop
    |-- GET /api/scores -->|                |
    |<-- scores + laptop_ip|                |
    |                      |                |
    |-- POST claim ------->|                |
    |<-- 200 OK            |                |
    |                      |-- POST claim ->|
    |                      |                |
    |-- POST photo ----------------------->|
    |<-------------------------- 200 OK ----|
    |                      |<- photo_ok ----|
    |                      |                |
    |-- GET photo ------------------------>|
    |<--------------------------- image ----|

Boot recovery (laptop present):

  Laptop             ESP32
    |-- POST register ->|
    |                   |-- GET history -->|
    |<-- scores + claims ------------------|
    |   (populate memory)                  |
```
