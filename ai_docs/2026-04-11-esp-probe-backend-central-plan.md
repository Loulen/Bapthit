# ESP sonde + backend central — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Bascule big-bang de l'architecture actuelle (ESP en SoftAP + HTTP server + UI embarquée) vers ESP sonde pure (STA + WebSocket) avec backend FastAPI central qui sert UI, API, DB, config et mDNS.

**Architecture:** ESP32 se connecte au WiFi maison en STA, résout `bapthit-server.local` via mDNS, se branche en WebSocket sur le backend et reste une sonde passive qui pousse des scores. Backend FastAPI héberge l'UI, stocke tout en SQLite, publie mDNS, et pousse la config au device via WS.

**Tech Stack:** ESP-IDF v6.0 (C/C++), `esp_websocket_client`, `mdns`, FreeRTOS queue; FastAPI, `websockets`, `zeroconf`, SQLite, Uvicorn.

**Design de référence:** `ai_docs/2026-04-11-esp-probe-backend-central-design.md`

**Pré-requis :**
- Pas de tests unitaires formels (non-goal du design).
- Validation bout-en-bout manuelle via `idf.py monitor`, `curl`, `wscat`, et Chrome MCP.
- Commits fréquents — un commit par tâche. Chaque tâche doit laisser l'arbre dans un état build-able (pour le ESP) ou run-able (pour le backend) autant que possible, sachant que big-bang implique une fenêtre où les deux côtés sont incohérents.

**Stratégie de branche :** Travailler sur une branche `feature/esp-probe-backend-central`. PR unique à la fin. Le firmware actuel reste flashable sur master pendant l'implémentation.

---

## Overview des tâches

1. **Phase 1 — Backend** (Tasks 1–10) : grossir `backend/server.py`, ajouter WS, config, mDNS, static.
2. **Phase 2 — UI** (Tasks 11–12) : déplacer `main/index.html` vers `backend/static/` et adapter les URLs.
3. **Phase 3 — Firmware ESP** (Tasks 13–21) : Kconfig creds, modules `wifi_sta`/`ws_client`/`score_queue`, réécriture de `main.cpp`.
4. **Phase 4 — Validation E2E** (Task 22) : flash, monitor, tests de bout en bout.

---

## Phase 1 — Backend

### Task 1: Préparer la branche et les dépendances Python

**Files:**
- Modify: `backend/requirements.txt`

**Step 1: Créer la branche**

```bash
git checkout -b feature/esp-probe-backend-central
```

**Step 2: Ajouter les dépendances**

Édite `backend/requirements.txt` pour ajouter `websockets` et `zeroconf` :

```
fastapi==0.115.*
uvicorn[standard]==0.34.*
httpx==0.28.*
python-multipart>=0.0.18
websockets==13.*
zeroconf==0.136.*
```

Note : `httpx` reste pour l'instant, on le retire en Task 10 une fois la logique de forwarding supprimée.

**Step 3: Installer dans le venv**

Run: `cd backend && source .venv/bin/activate && pip install -r requirements.txt`
Expected: installation OK, `pip show websockets zeroconf` renvoie les versions.

**Step 4: Commit**

```bash
git add backend/requirements.txt
git commit -m "backend: add websockets and zeroconf deps for probe redesign"
```

**Acceptance Criteria:**

- [ ] Run `pip show websockets zeroconf` dans `backend/.venv` — Expected: affiche version 13.x et 0.136.x
- [ ] Run `git log --oneline -1` — Expected: commit "backend: add websockets and zeroconf deps…"

---

### Task 2: Refactor `backend/server.py` — suppression du code legacy d'intégration ESP

**Files:**
- Modify: `backend/server.py`

**Step 1: Retirer `ESP32_IP`, `register_with_esp32`, `photo_ok` forward, lifespan socket detection**

Réécris le haut de `backend/server.py` pour retirer :
- La constante `ESP32_IP = "192.168.4.1"`
- La fonction `register_with_esp32`
- Le bloc `lifespan` qui fait `socket.connect((ESP32_IP, 80))`
- L'import `httpx` (plus utilisé)
- Le bloc `try: httpx.AsyncClient... /api/photo_ok` dans `upload_photo`
- La fonction `receive_score` (POST /api/scores) — sera remplacée par WS

À la fin de cette tâche, `backend/server.py` ressemble à :

```python
import sqlite3
from datetime import datetime
from pathlib import Path
from contextlib import asynccontextmanager

from fastapi import FastAPI, UploadFile, File, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse
from pydantic import BaseModel

PHOTOS_DIR = Path(__file__).parent / "photos"
DB_PATH = Path(__file__).parent / "bapthit.db"


def get_db():
    conn = sqlite3.connect(str(DB_PATH))
    conn.row_factory = sqlite3.Row
    return conn


def init_db():
    conn = get_db()
    conn.execute("""
        CREATE TABLE IF NOT EXISTS scores (
            id INTEGER PRIMARY KEY,
            hour INTEGER NOT NULL,
            minute INTEGER NOT NULL,
            score INTEGER NOT NULL,
            name TEXT NOT NULL DEFAULT '',
            has_photo INTEGER NOT NULL DEFAULT 0
        )
    """)
    conn.commit()
    conn.close()


@asynccontextmanager
async def lifespan(app: FastAPI):
    init_db()
    PHOTOS_DIR.mkdir(exist_ok=True)
    yield


app = FastAPI(lifespan=lifespan)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)


# --- Models ---

class ClaimIn(BaseModel):
    id: int
    name: str


# --- Endpoints ---

@app.post("/api/scores/claim")
def receive_claim(data: ClaimIn):
    conn = get_db()
    conn.execute(
        "UPDATE scores SET name = ? WHERE id = ?",
        (data.name, data.id),
    )
    conn.commit()
    conn.close()
    return {"ok": True}


@app.get("/api/scores/history")
def get_history():
    conn = get_db()
    rows = conn.execute(
        "SELECT id, hour, minute, score, name, has_photo FROM scores ORDER BY id"
    ).fetchall()
    conn.close()
    return [
        {
            "id": r["id"],
            "hour": r["hour"],
            "minute": r["minute"],
            "score": r["score"],
            "name": r["name"],
            "has_photo": bool(r["has_photo"]),
        }
        for r in rows
    ]


@app.post("/api/photos/{score_id}")
async def upload_photo(score_id: int, photo: UploadFile = File(...)):
    photo_path = PHOTOS_DIR / f"{score_id}.jpg"
    MAX_PHOTO_SIZE = 10 * 1024 * 1024
    contents = await photo.read(MAX_PHOTO_SIZE + 1)
    if len(contents) > MAX_PHOTO_SIZE:
        raise HTTPException(status_code=413, detail="Photo too large (max 10 MB)")
    photo_path.write_bytes(contents)

    conn = get_db()
    conn.execute("UPDATE scores SET has_photo = 1 WHERE id = ?", (score_id,))
    conn.commit()
    conn.close()

    return {"ok": True}


@app.get("/api/photos/{score_id}")
def get_photo(score_id: int):
    photo_path = PHOTOS_DIR / f"{score_id}.jpg"
    if not photo_path.exists():
        raise HTTPException(status_code=404, detail="Photo not found")
    return FileResponse(photo_path, media_type="image/jpeg")


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=6969)
```

**Step 2: Vérifier que le backend démarre**

Run: `cd backend && .venv/bin/uvicorn server:app --host 0.0.0.0 --port 6969`
Expected: `Application startup complete.` sans crash. `Ctrl-C` pour arrêter.

**Step 3: Commit**

```bash
git add backend/server.py
git commit -m "backend: drop ESP32 HTTP forwarding and legacy score ingress"
```

**Acceptance Criteria:**

- [ ] Run `grep -n "ESP32_IP\|register_with_esp32\|httpx" backend/server.py` — Expected: no matches
- [ ] Run `cd backend && .venv/bin/python -c "import server; print('ok')"` — Expected: prints `ok` sans exception
- [ ] Run `cd backend && .venv/bin/uvicorn server:app --port 6969 &` puis `curl http://localhost:6969/api/scores/history` — Expected: répond `[]` ou la liste existante. Kill le serveur ensuite.

---

### Task 3: Ajouter la table `config` + defaults hardcodés

**Files:**
- Modify: `backend/server.py`

**Step 1: Ajouter les defaults et init_db**

En haut de `server.py`, après `DB_PATH`, ajoute :

```python
DEFAULT_CONFIG = {
    "maxScore":         20000,
    "minScore":         100000,
    "defaultRollDelay": 15,
    "rollDelayMod":     1,
    "rollThresh":       60,
    "slowRollThresh":   7,
    "slowRollDelayMod": 100,
    "defaultIncrement": 15,
    "blinkDelay":       600,
    "waveDuration":     1500,
    "waveDelay":        200,
}
CONFIG_KEYS = list(DEFAULT_CONFIG.keys())
```

Les valeurs doivent **mirrorer exactement** celles dans `main/main.cpp` (struct `shared_config` ligne 61).

Dans `init_db()`, ajoute la table config et insère les defaults si vide :

```python
def init_db():
    conn = get_db()
    conn.execute("""
        CREATE TABLE IF NOT EXISTS scores (
            id INTEGER PRIMARY KEY,
            hour INTEGER NOT NULL,
            minute INTEGER NOT NULL,
            score INTEGER NOT NULL,
            name TEXT NOT NULL DEFAULT '',
            has_photo INTEGER NOT NULL DEFAULT 0
        )
    """)
    conn.execute("""
        CREATE TABLE IF NOT EXISTS config (
            key TEXT PRIMARY KEY,
            value INTEGER NOT NULL
        )
    """)
    # Seed defaults if empty
    count = conn.execute("SELECT COUNT(*) FROM config").fetchone()[0]
    if count == 0:
        conn.executemany(
            "INSERT INTO config (key, value) VALUES (?, ?)",
            [(k, v) for k, v in DEFAULT_CONFIG.items()],
        )
    conn.commit()
    conn.close()


def load_config() -> dict:
    """Load the full config from DB. Falls back to DEFAULT_CONFIG on corruption."""
    try:
        conn = get_db()
        rows = conn.execute("SELECT key, value FROM config").fetchall()
        conn.close()
        cfg = {r["key"]: r["value"] for r in rows}
        # Ensure all keys are present; fill missing with defaults
        for k, v in DEFAULT_CONFIG.items():
            cfg.setdefault(k, v)
        return cfg
    except Exception as e:
        print(f"Config load failed, using defaults: {e}")
        return dict(DEFAULT_CONFIG)


def save_config(updates: dict) -> dict:
    """Update the given keys in DB. Returns the full merged config."""
    conn = get_db()
    for k, v in updates.items():
        if k in DEFAULT_CONFIG:
            conn.execute(
                "INSERT INTO config (key, value) VALUES (?, ?) "
                "ON CONFLICT(key) DO UPDATE SET value = excluded.value",
                (k, int(v)),
            )
    conn.commit()
    conn.close()
    return load_config()
```

**Step 2: Vérifier que la table est créée**

Run:
```bash
rm backend/bapthit.db  # fresh start
cd backend && .venv/bin/uvicorn server:app --port 6969 &
sleep 1
sqlite3 backend/bapthit.db "SELECT * FROM config"
pkill -f "uvicorn server:app"
```
Expected: 11 lignes, `maxScore|20000`, etc.

**Step 3: Commit**

```bash
git add backend/server.py
git commit -m "backend: add config table with PunchMeter defaults seed"
```

**Acceptance Criteria:**

- [ ] Run `sqlite3 backend/bapthit.db "SELECT COUNT(*) FROM config"` — Expected: `11`
- [ ] Run `sqlite3 backend/bapthit.db "SELECT value FROM config WHERE key='maxScore'"` — Expected: `20000`

---

### Task 4: GET/POST `/api/config` (DB only, no device push yet)

**Files:**
- Modify: `backend/server.py`

**Step 1: Ajouter les endpoints**

Ajoute dans la section Endpoints de `server.py` :

```python
@app.get("/api/config")
def get_config():
    return load_config()


@app.post("/api/config")
async def update_config(payload: dict):
    # Validate: only known keys, ints only
    filtered = {}
    for k in CONFIG_KEYS:
        if k in payload:
            try:
                filtered[k] = int(payload[k])
            except (TypeError, ValueError):
                raise HTTPException(status_code=400, detail=f"Invalid value for {k}")
    if not filtered:
        raise HTTPException(status_code=400, detail="No valid keys")
    cfg = save_config(filtered)
    # DeviceManager push will be wired in Task 7
    return cfg
```

**Step 2: Vérifier manuellement**

Run:
```bash
cd backend && .venv/bin/uvicorn server:app --port 6969 &
sleep 1
curl -s http://localhost:6969/api/config | head -c 200
curl -s -X POST -H "Content-Type: application/json" \
  -d '{"maxScore": 25000}' http://localhost:6969/api/config | head -c 200
pkill -f "uvicorn server:app"
```
Expected: GET renvoie le JSON config, POST renvoie la config mise à jour avec `maxScore: 25000`.

**Step 3: Commit**

```bash
git add backend/server.py
git commit -m "backend: add GET/POST /api/config endpoints backed by SQLite"
```

**Acceptance Criteria:**

- [ ] Run `curl -s http://localhost:6969/api/config | python -c "import sys,json; d=json.load(sys.stdin); print(d['maxScore'])"` — Expected: `25000` (après le POST ci-dessus)
- [ ] Run `curl -s -X POST -H "Content-Type: application/json" -d '{"bogus": 1}' http://localhost:6969/api/config -o /dev/null -w "%{http_code}"` — Expected: `400`

---

### Task 5: WebSocket `/ws/device` + `DeviceManager` singleton

**Files:**
- Modify: `backend/server.py`

**Step 1: Ajouter DeviceManager et l'endpoint WebSocket**

Ajoute en haut avec les autres imports :
```python
import asyncio
import json
from fastapi import WebSocket, WebSocketDisconnect
```

Ajoute après la classe `ClaimIn` :

```python
# --- Device WebSocket manager ---

class DeviceManager:
    """Singleton-ish holder for the single connected ESP device."""
    def __init__(self):
        self._ws: WebSocket | None = None
        self._lock = asyncio.Lock()

    async def attach(self, ws: WebSocket):
        async with self._lock:
            if self._ws is not None:
                try:
                    await self._ws.close()
                except Exception:
                    pass
            self._ws = ws

    async def detach(self, ws: WebSocket):
        async with self._lock:
            if self._ws is ws:
                self._ws = None

    def is_connected(self) -> bool:
        return self._ws is not None

    async def send_json(self, msg: dict) -> bool:
        ws = self._ws
        if ws is None:
            return False
        try:
            await ws.send_text(json.dumps(msg))
            return True
        except Exception as e:
            print(f"DeviceManager send failed: {e}")
            return False


device_manager = DeviceManager()
```

Ajoute l'endpoint WS :

```python
@app.websocket("/ws/device")
async def device_ws(ws: WebSocket):
    await ws.accept()
    await device_manager.attach(ws)
    print("Device connected")
    try:
        while True:
            raw = await ws.receive_text()
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError:
                print(f"Device: bad JSON: {raw[:80]}")
                continue
            # Message routing added in Task 6
            mtype = msg.get("type")
            print(f"Device msg: {mtype}")
    except WebSocketDisconnect:
        print("Device disconnected")
    finally:
        await device_manager.detach(ws)
```

**Step 2: Vérifier avec wscat**

Assure-toi que `wscat` est installé : `which wscat || npm install -g wscat`.

Run:
```bash
cd backend && .venv/bin/uvicorn server:app --port 6969 &
sleep 1
echo '{"type":"hello","fw":"test"}' | wscat -c ws://localhost:6969/ws/device -x '{"type":"hello","fw":"test"}' -w 1
pkill -f "uvicorn server:app"
```
Expected : les logs uvicorn affichent `Device connected`, `Device msg: hello`, puis `Device disconnected`.

**Step 3: Commit**

```bash
git add backend/server.py
git commit -m "backend: add /ws/device WebSocket endpoint and DeviceManager"
```

**Acceptance Criteria:**

- [ ] Run `wscat -c ws://localhost:6969/ws/device` — Expected: la console uvicorn affiche `Device connected`
- [ ] Taper `{"type":"hello","fw":"test"}` dans wscat — Expected: uvicorn affiche `Device msg: hello`
- [ ] Fermer wscat (Ctrl-C) — Expected: uvicorn affiche `Device disconnected`

---

### Task 6: Routage des messages WS (hello, score, log) + log buffer

**Files:**
- Modify: `backend/server.py`

**Step 1: Buffer de logs et insertion de scores**

Ajoute après `DEFAULT_CONFIG` :

```python
from collections import deque

LOG_BUFFER_MAX = 500
log_buffer: deque = deque(maxlen=LOG_BUFFER_MAX)
log_seq = 0
```

Ajoute une fonction utilitaire pour insérer un score :

```python
def insert_score(id_: int, score: int, ts_epoch: int) -> None:
    dt = datetime.fromtimestamp(ts_epoch) if ts_epoch > 0 else datetime.now()
    conn = get_db()
    conn.execute(
        "INSERT OR IGNORE INTO scores (id, hour, minute, score) VALUES (?, ?, ?, ?)",
        (id_, dt.hour, dt.minute, score),
    )
    conn.commit()
    conn.close()
```

**Step 2: Routage dans `device_ws`**

Remplace `# Message routing added in Task 6` par :

```python
            if mtype == "hello":
                fw = msg.get("fw", "?")
                print(f"Device hello: fw={fw}")
                # Push current config to device
                await device_manager.send_json({"type": "config", **load_config()})
            elif mtype == "score":
                try:
                    sid = int(msg["id"])
                    sval = int(msg["score"])
                    sts = int(msg.get("ts", 0))
                except (KeyError, ValueError, TypeError):
                    print(f"Device: bad score msg: {msg}")
                    continue
                insert_score(sid, sval, sts)
                print(f"Device score: id={sid} val={sval}")
            elif mtype == "log":
                global log_seq
                log_seq += 1
                log_buffer.append({
                    "s": log_seq,
                    "t": int(datetime.now().timestamp() * 1000),
                    "level": msg.get("level", "I"),
                    "m": str(msg.get("msg", ""))[:200],
                })
            else:
                print(f"Device: unknown msg type: {mtype}")
```

**Step 3: Vérifier avec wscat**

Run:
```bash
cd backend && .venv/bin/uvicorn server:app --port 6969 &
sleep 1
# Connect and send hello, expect config echoed back
wscat -c ws://localhost:6969/ws/device -x '{"type":"hello","fw":"test"}' -w 2
# Send a score
wscat -c ws://localhost:6969/ws/device -x '{"type":"score","id":999,"score":777,"ts":0}' -w 1
sqlite3 backend/bapthit.db "SELECT id, score FROM scores WHERE id=999"
pkill -f "uvicorn server:app"
```
Expected : `wscat` reçoit un message `{"type":"config",...}` après le hello, et `sqlite3` renvoie `999|777`.

**Step 4: Commit**

```bash
git add backend/server.py
git commit -m "backend: route ws device messages (hello/score/log)"
```

**Acceptance Criteria:**

- [ ] Run `wscat -c ws://localhost:6969/ws/device -x '{"type":"hello","fw":"test"}' -w 2` — Expected: reçoit `{"type":"config",...}` avec les 11 clés
- [ ] Run `wscat -c ws://localhost:6969/ws/device -x '{"type":"score","id":999,"score":777,"ts":0}' -w 1; sqlite3 backend/bapthit.db "SELECT score FROM scores WHERE id=999"` — Expected: `777`
- [ ] Run `wscat -c ws://localhost:6969/ws/device -x '{"type":"log","level":"I","msg":"hello"}' -w 1` puis vérifier que le buffer se remplit (sera exposé au Task 8)

---

### Task 7: POST `/api/config` pousse la config au device

**Files:**
- Modify: `backend/server.py`

**Step 1: Wire up send to device**

Remplace le commentaire `# DeviceManager push will be wired in Task 7` dans `update_config` par :

```python
    # Push to device if connected
    if device_manager.is_connected():
        await device_manager.send_json({"type": "config", **cfg})
```

**Step 2: Vérifier manuellement**

Run (dans deux terminaux) :

```bash
# Terminal 1
cd backend && .venv/bin/uvicorn server:app --port 6969
# Terminal 2
wscat -c ws://localhost:6969/ws/device
# (in wscat prompt) > {"type":"hello","fw":"test"}
# Terminal 3
curl -s -X POST -H "Content-Type: application/json" \
  -d '{"maxScore": 30000}' http://localhost:6969/api/config
```
Expected : wscat reçoit un premier config (après hello) puis un deuxième avec `maxScore:30000`.

**Step 3: Commit**

```bash
git add backend/server.py
git commit -m "backend: push config update to device over ws on POST /api/config"
```

**Acceptance Criteria:**

- [ ] Connect wscat à `/ws/device`, envoyer hello, observer le premier `config` reçu
- [ ] Depuis un autre terminal, `curl -X POST -d '{"maxScore": 30000}' …/api/config` — Expected: wscat reçoit un nouveau `{"type":"config",...,"maxScore":30000,...}`

---

### Task 8: `GET /api/logs` renvoie le buffer

**Files:**
- Modify: `backend/server.py`

**Step 1: Ajouter l'endpoint**

```python
@app.get("/api/logs")
def get_logs(since: int = 0):
    return {"logs": [e for e in log_buffer if e["s"] > since]}
```

**Step 2: Vérifier**

Run :
```bash
cd backend && .venv/bin/uvicorn server:app --port 6969 &
sleep 1
wscat -c ws://localhost:6969/ws/device -x '{"type":"log","level":"I","msg":"hello"}' -w 1
curl -s http://localhost:6969/api/logs
pkill -f "uvicorn server:app"
```
Expected : la réponse contient une liste avec un objet `{s, t, level, m: "hello"}`.

**Step 3: Commit**

```bash
git add backend/server.py
git commit -m "backend: expose GET /api/logs with since filter"
```

**Acceptance Criteria:**

- [ ] Run `curl -s "http://localhost:6969/api/logs?since=0" | python -c "import sys,json; d=json.load(sys.stdin); print(len(d['logs']))"` — Expected: >= 1 si un log a été envoyé via wscat
- [ ] Run `curl -s "http://localhost:6969/api/logs?since=99999"` — Expected: `{"logs":[]}`

---

### Task 9: mDNS publisher via `zeroconf` dans le lifespan

**Files:**
- Modify: `backend/server.py`

**Step 1: Ajouter le publisher**

En haut, avec les imports :
```python
import socket as _socket
from zeroconf.asyncio import AsyncZeroconf
from zeroconf import ServiceInfo
```

Remplace la fonction `lifespan` par :

```python
MDNS_SERVICE_TYPE = "_bapthit._tcp.local."
MDNS_NAME = "bapthit-server"
MDNS_PORT = 6969


def _local_ip() -> str:
    s = _socket.socket(_socket.AF_INET, _socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()


@asynccontextmanager
async def lifespan(app: FastAPI):
    init_db()
    PHOTOS_DIR.mkdir(exist_ok=True)

    ip = _local_ip()
    print(f"Publishing mDNS: {MDNS_NAME}.local -> {ip}:{MDNS_PORT}")
    aiozc = AsyncZeroconf()
    info = ServiceInfo(
        MDNS_SERVICE_TYPE,
        f"{MDNS_NAME}.{MDNS_SERVICE_TYPE}",
        addresses=[_socket.inet_aton(ip)],
        port=MDNS_PORT,
        server=f"{MDNS_NAME}.local.",
        properties={},
    )
    await aiozc.async_register_service(info)
    try:
        yield
    finally:
        await aiozc.async_unregister_service(info)
        await aiozc.async_close()
```

**Step 2: Vérifier**

Run :
```bash
cd backend && .venv/bin/uvicorn server:app --port 6969 &
sleep 2
avahi-browse -art -p | grep -i bapthit
pkill -f "uvicorn server:app"
```
Expected : au moins une ligne qui inclut `bapthit-server._bapthit._tcp`.

Note : si `avahi-browse` n'est pas installé, `sudo apt install avahi-utils`. Sinon, ping `bapthit-server.local` depuis une autre machine du même LAN.

**Step 3: Commit**

```bash
git add backend/server.py
git commit -m "backend: publish _bapthit._tcp via zeroconf on startup"
```

**Acceptance Criteria:**

- [ ] Run `avahi-browse -art -p | grep bapthit` avec le serveur en route — Expected: la ligne contient `bapthit-server`
- [ ] Run `ping -c 1 bapthit-server.local` — Expected: résolution en l'IP locale

---

### Task 10: Servir l'UI depuis `backend/static/` et `GET /`

**Files:**
- Modify: `backend/server.py`
- Create: `backend/static/.gitkeep`

**Step 1: Créer le dossier static**

```bash
mkdir -p backend/static
touch backend/static/.gitkeep
```

**Step 2: Monter StaticFiles et `GET /`**

En haut, imports :
```python
from fastapi.staticfiles import StaticFiles
```

À la fin de `server.py`, **avant** `if __name__ == "__main__"`, ajoute :

```python
STATIC_DIR = Path(__file__).parent / "static"


@app.get("/")
def root():
    index = STATIC_DIR / "index.html"
    if not index.exists():
        raise HTTPException(status_code=404, detail="UI not built")
    return FileResponse(index, media_type="text/html")


app.mount("/static", StaticFiles(directory=str(STATIC_DIR)), name="static")
```

**Step 3: Vérifier**

Run :
```bash
cd backend && .venv/bin/uvicorn server:app --port 6969 &
sleep 1
# 404 expected for now (index.html not yet copied)
curl -s -o /dev/null -w "%{http_code}" http://localhost:6969/
pkill -f "uvicorn server:app"
```
Expected : `404` (c'est ok, on copiera l'UI en Task 11).

**Step 4: Commit**

```bash
git add backend/server.py backend/static/.gitkeep
git commit -m "backend: serve backend/static and GET / (index.html)"
```

**Acceptance Criteria:**

- [ ] Run `curl -s -o /dev/null -w "%{http_code}" http://localhost:6969/` — Expected: `404` (pas encore d'index.html)
- [ ] Run `ls backend/static/` — Expected: `.gitkeep` présent

---

## Phase 2 — Web UI

### Task 11: Déplacer `main/index.html` vers `backend/static/index.html`

**Files:**
- Move: `main/index.html` → `backend/static/index.html`

**Step 1: Git mv pour préserver l'historique**

```bash
git mv main/index.html backend/static/index.html
```

**Step 2: Vérifier que la page se sert**

Run :
```bash
cd backend && .venv/bin/uvicorn server:app --port 6969 &
sleep 1
curl -s -o /dev/null -w "%{http_code}" http://localhost:6969/
curl -s http://localhost:6969/ | head -c 200
pkill -f "uvicorn server:app"
```
Expected : `200` et les premières lignes du HTML.

**Step 3: Commit**

```bash
git add backend/static/index.html main/index.html
git commit -m "ui: move index.html from ESP embed to backend/static"
```

**Acceptance Criteria:**

- [ ] Run `test -f backend/static/index.html && echo ok` — Expected: `ok`
- [ ] Run `test -f main/index.html && echo exists || echo gone` — Expected: `gone`
- [ ] Run `curl -s -o /dev/null -w "%{http_code}" http://localhost:6969/` — Expected: `200`

---

### Task 12: Adapter les URLs et la logique de l'UI

**Files:**
- Modify: `backend/static/index.html`

**Changes nécessaires :** tous les `fetch`/XHR doivent pointer sur le backend (même origine), et l'UI doit consommer le nouveau shape d'API. Les valeurs de `laptop_ip` et "backend status" sont supprimées.

**Step 1: Remplacer `fetch('/api/scores')` par `fetch('/api/scores/history')` et gérer le nouveau shape**

Autour des lignes 1311–1340 dans `backend/static/index.html`, remplace la fonction `fetchScores` par :

```javascript
async function fetchScores() {
  try {
    const res = await fetch('/api/scores/history');
    if (!res.ok) return;
    const rows = await res.json();
    // Normalize to the old shape used by renderScores/drawChart
    allScores = rows.map(r => ({
      id: r.id,
      time: String(r.hour).padStart(2, '0') + 'h' + String(r.minute).padStart(2, '0'),
      score: r.score,
      name: r.name,
      has_photo: r.has_photo,
    }));
    renderScores(allScores);
    if (document.getElementById('stats-panel').classList.contains('active')) {
      drawChart();
    }
  } catch (e) {
    // silently retry on next poll
  }
}
```

**Step 2: Retirer toute référence à `laptopIp`**

Dans `backend/static/index.html`, les URLs de photos utilisaient `http://laptopIp:6969/api/photos/...`. Maintenant, même origine → juste `/api/photos/...`.

Remplace `grep` tous les endroits `laptopIp` et adapte :

- Lignes ~1045, ~1071 (src d'avatar dans `renderScores`): remplacer `'http://' + laptopIp + ':6969/api/photos/' + s.id` par `'/api/photos/' + s.id`.
- Ligne ~1290 (claim photo upload): remplacer `'http://' + laptopIp + ':6969/api/photos/'` par `'/api/photos/'` et retirer le `if (… && laptopIp)` — l'upload est inconditionnel quand une photo est sélectionnée.
- Déclaration `let laptopIp = null;` → supprimer.

**Step 3: Retirer le backend status indicator**

Le bloc `const dot = document.getElementById('backend-dot'); ...` dans `fetchScores` est déjà supprimé à l'étape 1. Il faut aussi retirer les éléments HTML :

- Dans le HTML (panneau settings), supprimer `<div id="backend-dot">`, `<span id="backend-text">` et le label "Backend portable" (grep `backend-dot` et `backend-text`).
- Leur CSS peut rester en place, ça n'a pas d'impact fonctionnel.

**Step 4: Adapter `/api/claim` → `/api/scores/claim`**

Ligne 1274 : `fetch('/api/claim', {...})` → `fetch('/api/scores/claim', {...})`.

**Step 5: Retirer l'UI OTA (hors scope de ce sprint)**

ESP n'a plus de serveur HTTP → on retire l'UI OTA, qu'on pourra remettre plus tard côté backend si besoin. Supprimer :
- La zone `<div id="ota-zone">` et tous ses éléments (drop zone, progress bar, status)
- Le bloc de script `const otaZone = ...` et la fonction `startOta(...)` (lignes ~1518–1582)
- Le titre/label "Mise à jour OTA" dans le panneau settings

**Step 6: Vérifier visuellement via Chrome DevTools MCP**

Run :
```bash
cd backend && .venv/bin/uvicorn server:app --port 6969 &
sleep 1
```

Depuis Chrome DevTools MCP :
1. `navigate_page` → `http://localhost:6969/`
2. `list_console_messages` — Expected: pas d'erreur JS
3. `take_snapshot` — Expected: voir l'UI, scores depuis la DB
4. Ouvrir le panneau settings (cliquer sur gear) — Expected: pas de crash, config se charge

Kill le serveur ensuite.

**Step 7: Commit**

```bash
git add backend/static/index.html
git commit -m "ui: point API calls to backend same-origin and drop ESP-specific UI"
```

**Acceptance Criteria:**

- [ ] Run `grep -n "laptopIp\|/api/claim\b\|/api/ota" backend/static/index.html` — Expected: no matches
- [ ] Run `grep -n "/api/scores/history\|/api/scores/claim" backend/static/index.html` — Expected: matches present
- [ ] Load `http://localhost:6969/` dans un navigateur — Expected: page charge, scores visibles, aucun onerror sur fetch, panneau config s'ouvre et affiche les 11 champs.

---

## Phase 3 — Firmware ESP

### Task 13: Préparation — `sdkconfig.defaults.local` gitignored + template

**Files:**
- Modify: `.gitignore`
- Create: `sdkconfig.defaults.local.example`

**Step 1: Gitignore**

Ajoute à `.gitignore` :
```
sdkconfig.defaults.local
```

**Step 2: Créer un template**

Create `sdkconfig.defaults.local.example` :
```
# Copy to sdkconfig.defaults.local and fill in real creds.
# This file is read by ESP-IDF build system for default Kconfig values.
CONFIG_BAPTHIT_WIFI_SSID="your-home-wifi"
CONFIG_BAPTHIT_WIFI_PASS="your-wpa2-password"
CONFIG_BAPTHIT_BACKEND_HOST="bapthit-server.local"
CONFIG_BAPTHIT_BACKEND_PORT=6969
```

**Step 3: Commit**

```bash
git add .gitignore sdkconfig.defaults.local.example
git commit -m "esp: add sdkconfig.defaults.local template for wifi/backend creds"
```

**Acceptance Criteria:**

- [ ] Run `grep "sdkconfig.defaults.local" .gitignore` — Expected: match found
- [ ] Run `test -f sdkconfig.defaults.local.example && echo ok` — Expected: `ok`

---

### Task 14: `Kconfig.projbuild` pour les credentials

**Files:**
- Create: `main/Kconfig.projbuild`
- Create: `sdkconfig.defaults.local` (locally, gitignored)

**Step 1: Créer le Kconfig.projbuild**

Create `main/Kconfig.projbuild` :

```
menu "Bapthit Configuration"

config BAPTHIT_WIFI_SSID
    string "Home WiFi SSID"
    default "bapthit-missing-ssid"
    help
        SSID of the station WiFi network the ESP will join.

config BAPTHIT_WIFI_PASS
    string "Home WiFi password"
    default "bapthit-missing-pass"
    help
        WPA2 PSK for the station WiFi. Keep in sdkconfig.defaults.local (gitignored).

config BAPTHIT_BACKEND_HOST
    string "Backend mDNS hostname"
    default "bapthit-server.local"
    help
        Hostname resolved via mDNS for the laptop FastAPI backend.

config BAPTHIT_BACKEND_PORT
    int "Backend TCP port"
    default 6969
    range 1 65535

endmenu
```

**Step 2: Créer `sdkconfig.defaults.local` localement**

```bash
cp sdkconfig.defaults.local.example sdkconfig.defaults.local
# ✏️  Remplis avec les vraies creds WiFi maison
```

**Step 3: Configurer le build pour charger le .local**

Modify `CMakeLists.txt` (root) pour inclure le fichier local en plus des defaults :

```cmake
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/components/punchmeter/idf_component"
)

# Load local credentials overlay if present
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults.local")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.local")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bapthit)
```

**Step 4: Vérifier que Kconfig est lu**

Run :
```bash
source ~/.espressif/v6.0/esp-idf/export.sh
idf.py reconfigure
grep "CONFIG_BAPTHIT_WIFI_SSID" sdkconfig
```
Expected : une ligne `CONFIG_BAPTHIT_WIFI_SSID="..."` avec la valeur du `.local`.

**Step 5: Commit**

```bash
git add main/Kconfig.projbuild CMakeLists.txt
git commit -m "esp: add Kconfig for wifi/backend creds with sdkconfig.defaults.local overlay"
```

**Acceptance Criteria:**

- [ ] Run `idf.py reconfigure` — Expected: no Kconfig errors
- [ ] Run `grep "CONFIG_BAPTHIT_WIFI_SSID" sdkconfig` — Expected: line with SSID from `.local`
- [ ] Run `grep "CONFIG_BAPTHIT_BACKEND_PORT=6969" sdkconfig` — Expected: match

---

### Task 15: Ajouter les managed_components `esp_websocket_client` + `mdns`

**Files:**
- Create or Modify: `main/idf_component.yml`

**Step 1: Créer/mettre à jour `main/idf_component.yml`**

Write `main/idf_component.yml` :

```yaml
dependencies:
  espressif/esp_websocket_client: "^1.4.0"
  espressif/mdns: "^1.8.0"
  idf: ">=5.0"
```

**Step 2: Reconfigure pour déclencher le fetch**

Run :
```bash
idf.py reconfigure
ls managed_components/
```
Expected : les dossiers `espressif__esp_websocket_client` et `espressif__mdns` apparaissent.

**Step 3: Commit**

```bash
git add main/idf_component.yml dependencies.lock
git commit -m "esp: pull in esp_websocket_client and mdns managed components"
```

**Acceptance Criteria:**

- [ ] Run `ls managed_components/ | grep -E "websocket|mdns"` — Expected: both listed
- [ ] Run `test -f dependencies.lock && echo ok` — Expected: `ok`

---

### Task 16: Module `wifi_sta.c/h`

**Files:**
- Create: `main/wifi_sta.h`
- Create: `main/wifi_sta.c`
- Modify: `main/CMakeLists.txt`

**Step 1: Créer `main/wifi_sta.h`**

```c
#ifndef WIFI_STA_H
#define WIFI_STA_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start WiFi in STA mode using CONFIG_BAPTHIT_WIFI_SSID/PASS.
 * Blocks until an IP is obtained, with infinite retries.
 */
esp_err_t wifi_sta_start_and_wait(void);

#ifdef __cplusplus
}
#endif

#endif
```

**Step 2: Créer `main/wifi_sta.c`**

```c
#include "wifi_sta.h"
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"

static const char *TAG = "wifi_sta";

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_evt = NULL;

static void evt_handler(void *arg, esp_event_base_t base,
                        int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Disconnected, retrying");
        xEventGroupClearBits(s_evt, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_evt, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_sta_start_and_wait(void)
{
    s_evt = xEventGroupCreate();

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &evt_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &evt_handler, NULL, NULL));

    wifi_config_t wc = {};
    strncpy((char *)wc.sta.ssid, CONFIG_BAPTHIT_WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, CONFIG_BAPTHIT_WIFI_PASS, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Waiting for IP on SSID '%s'...", CONFIG_BAPTHIT_WIFI_SSID);
    xEventGroupWaitBits(s_evt, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    return ESP_OK;
}
```

**Step 3: Mettre à jour `main/CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS "main.cpp" "wifi_sta.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event nvs_flash esp_netif esp_app_format app_update lwip idf_component
)

if(DEFINED ENV{BAPTHIT_DEV})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BAPTHIT_DEV=1)
endif()
```

Note : `esp_http_server`, `esp_http_client` ont été retirés de REQUIRES — on ne les utilise plus. `EMBED_TXTFILES "index.html"` aussi (sera retiré en Task 20 avec la suppression complète de main.cpp legacy, on anticipe ici car le fichier n'est plus dans `main/` après Task 11).

**Step 4: Vérifier que ça compile en isolation (avant réécriture de main.cpp, le module doit au moins compiler)**

Pour cette étape, on ne va pas déjà casser `main.cpp`. On ajoute juste `wifi_sta.c` sans le référencer depuis main.cpp, donc le build doit passer.

Run :
```bash
idf.py build 2>&1 | tail -40
```
Expected : build passes (warning sur `wifi_sta_start_and_wait` défini mais non utilisé est ok).

**Step 5: Commit**

```bash
git add main/wifi_sta.h main/wifi_sta.c main/CMakeLists.txt
git commit -m "esp: add wifi_sta module (STA connect + wait-for-IP)"
```

**Acceptance Criteria:**

- [ ] Run `idf.py build 2>&1 | grep -E "error:"` — Expected: no matches
- [ ] Run `ls main/wifi_sta.{h,c}` — Expected: both exist

---

### Task 17: Module `score_queue.c/h` — FIFO 64 slots drop-oldest

**Files:**
- Create: `main/score_queue.h`
- Create: `main/score_queue.c`
- Modify: `main/CMakeLists.txt`

**Step 1: Header**

```c
#ifndef SCORE_QUEUE_H
#define SCORE_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCORE_QUEUE_CAPACITY 64

typedef struct {
    int     id;
    int     score;
    int64_t ts_epoch;  // unix seconds, 0 if unknown
} score_item_t;

/** Initialize the singleton queue. Must be called once at boot. */
void score_queue_init(void);

/**
 * Push a score. If the queue is full, drops the oldest to make room
 * and logs a warning. Always succeeds.
 */
void score_queue_push(const score_item_t *item);

/**
 * Peek the oldest item without removing it. Returns false if empty.
 */
bool score_queue_peek(score_item_t *out);

/** Remove the oldest item. No-op if empty. */
void score_queue_pop(void);

/** Number of items currently buffered. */
int score_queue_count(void);

#ifdef __cplusplus
}
#endif

#endif
```

**Step 2: Source**

```c
#include "score_queue.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "score_q";

static score_item_t s_buf[SCORE_QUEUE_CAPACITY];
static int s_head = 0;   // index of oldest
static int s_count = 0;
static SemaphoreHandle_t s_mux = NULL;

void score_queue_init(void)
{
    s_mux = xSemaphoreCreateMutex();
    s_head = 0;
    s_count = 0;
}

void score_queue_push(const score_item_t *item)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    int tail = (s_head + s_count) % SCORE_QUEUE_CAPACITY;
    if (s_count == SCORE_QUEUE_CAPACITY) {
        ESP_LOGW(TAG, "queue full, dropping oldest id=%d", s_buf[s_head].id);
        s_buf[s_head] = *item;
        s_head = (s_head + 1) % SCORE_QUEUE_CAPACITY;
    } else {
        s_buf[tail] = *item;
        s_count++;
    }
    xSemaphoreGive(s_mux);
}

bool score_queue_peek(score_item_t *out)
{
    bool ok = false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_count > 0) {
        *out = s_buf[s_head];
        ok = true;
    }
    xSemaphoreGive(s_mux);
    return ok;
}

void score_queue_pop(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_count > 0) {
        s_head = (s_head + 1) % SCORE_QUEUE_CAPACITY;
        s_count--;
    }
    xSemaphoreGive(s_mux);
}

int score_queue_count(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    int n = s_count;
    xSemaphoreGive(s_mux);
    return n;
}
```

**Step 3: CMakeLists**

```cmake
idf_component_register(
    SRCS "main.cpp" "wifi_sta.c" "score_queue.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event nvs_flash esp_netif esp_app_format app_update lwip idf_component
)

if(DEFINED ENV{BAPTHIT_DEV})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BAPTHIT_DEV=1)
endif()
```

**Step 4: Build**

Run : `idf.py build 2>&1 | tail -20`
Expected : build passes.

**Step 5: Commit**

```bash
git add main/score_queue.h main/score_queue.c main/CMakeLists.txt
git commit -m "esp: add score_queue ring buffer (64 slots, drop-oldest)"
```

**Acceptance Criteria:**

- [ ] Run `idf.py build 2>&1 | grep "error:"` — Expected: no matches
- [ ] Run `grep -c "score_queue" main/CMakeLists.txt` — Expected: >= 1

---

### Task 18: Module `ws_client.c/h` — wrapper `esp_websocket_client`

**Files:**
- Create: `main/ws_client.h`
- Create: `main/ws_client.c`
- Modify: `main/CMakeLists.txt`

**Step 1: Header**

```c
#ifndef WS_CLIENT_H
#define WS_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ws_on_text_fn)(const char *data, size_t len);
typedef void (*ws_on_connect_fn)(void);

typedef struct {
    const char *host;   /* e.g. "bapthit-server.local" */
    int         port;   /* e.g. 6969 */
    const char *path;   /* e.g. "/ws/device" */
    ws_on_connect_fn on_connect;
    ws_on_text_fn    on_text;
} ws_client_cfg_t;

/** Start the WS client. Auto-reconnect is handled internally. */
esp_err_t ws_client_start(const ws_client_cfg_t *cfg);

/** True if the WS is currently connected (last known state). */
bool ws_client_is_connected(void);

/** Send a UTF-8 text frame (JSON). Returns ESP_OK on success. */
esp_err_t ws_client_send_text(const char *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
```

**Step 2: Source**

```c
#include "ws_client.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_websocket_client.h"

static const char *TAG = "ws_client";

static esp_websocket_client_handle_t s_client = NULL;
static ws_on_text_fn s_on_text = NULL;
static ws_on_connect_fn s_on_connect = NULL;
static volatile bool s_connected = false;

static void ws_event_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "CONNECTED");
        s_connected = true;
        if (s_on_connect) s_on_connect();
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "DISCONNECTED");
        s_connected = false;
        break;
    case WEBSOCKET_EVENT_DATA:
        if (d->op_code == 0x1 /* text */ && d->data_len > 0 && s_on_text) {
            s_on_text((const char *)d->data_ptr, d->data_len);
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "ERROR");
        s_connected = false;
        break;
    default:
        break;
    }
}

esp_err_t ws_client_start(const ws_client_cfg_t *cfg)
{
    char uri[128];
    snprintf(uri, sizeof(uri), "ws://%s:%d%s", cfg->host, cfg->port, cfg->path);

    esp_websocket_client_config_t wc = {};
    wc.uri = uri;
    wc.reconnect_timeout_ms = 5000;
    wc.network_timeout_ms = 10000;

    s_client = esp_websocket_client_init(&wc);
    if (!s_client) return ESP_FAIL;

    s_on_text = cfg->on_text;
    s_on_connect = cfg->on_connect;

    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    return esp_websocket_client_start(s_client);
}

bool ws_client_is_connected(void)
{
    return s_connected;
}

esp_err_t ws_client_send_text(const char *data, size_t len)
{
    if (!s_client || !s_connected) return ESP_FAIL;
    int sent = esp_websocket_client_send_text(s_client, data, len, pdMS_TO_TICKS(3000));
    return sent == (int)len ? ESP_OK : ESP_FAIL;
}
```

**Step 3: CMakeLists**

```cmake
idf_component_register(
    SRCS "main.cpp" "wifi_sta.c" "score_queue.c" "ws_client.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event nvs_flash esp_netif esp_app_format app_update lwip idf_component
)

if(DEFINED ENV{BAPTHIT_DEV})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BAPTHIT_DEV=1)
endif()
```

Note : `esp_websocket_client` est un managed_component, pas besoin de l'ajouter à REQUIRES, il est auto-linké.

**Step 4: Build**

Run : `idf.py build 2>&1 | tail -20`
Expected : build passes.

**Step 5: Commit**

```bash
git add main/ws_client.h main/ws_client.c main/CMakeLists.txt
git commit -m "esp: add ws_client wrapper over esp_websocket_client"
```

**Acceptance Criteria:**

- [ ] Run `idf.py build 2>&1 | grep "error:"` — Expected: no matches

---

### Task 19: Réécriture complète de `main.cpp`

**Files:**
- Modify: `main/main.cpp`

**Step 1: Remplacer le contenu intégral**

Overwrite `main/main.cpp` avec :

```cpp
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "mdns.h"
#include "sdkconfig.h"

#include "punchmeter_api.h"
#include "wifi_sta.h"
#include "score_queue.h"
#include "ws_client.h"

static const char *TAG = "bapthit";

// ---------------------------------------------------------------------------
// Shared PunchMeter config protected by mutex
// ---------------------------------------------------------------------------

typedef struct {
    PunchmeterConfig cfg;
    bool dirty;
    SemaphoreHandle_t mux;
} shared_cfg_t;

static shared_cfg_t s_cfg = {
    .cfg = {
        .maxScore         = 20000,
        .minScore         = 100000,
        .defaultRollDelay = 15,
        .rollDelayMod     = 1,
        .rollThresh       = 60,
        .slowRollThresh   = 7,
        .slowRollDelayMod = 100,
        .defaultIncrement = 15,
        .blinkDelay       = 600,
        .waveDuration     = 1500,
        .waveDelay        = 200,
    },
    .dirty = true,  // apply defaults once at boot
    .mux = NULL,
};

static int s_next_score_id = 1;

// ---------------------------------------------------------------------------
// Punchmeter log sink — forward to ESP_LOG + optionally WS
// ---------------------------------------------------------------------------

static void pm_log_sink(const char *msg)
{
    ESP_LOGI("PM", "%s", msg);
    // Optional: push as {"type":"log",...} — left for later if bandwidth ok
}

// ---------------------------------------------------------------------------
// JSON helpers (tiny parsers reused from legacy main.cpp)
// ---------------------------------------------------------------------------

static bool json_parse_int(const char *buf, const char *key, int *out)
{
    char needle[40];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *k = strstr(buf, needle);
    if (!k) return false;
    const char *colon = strchr(k, ':');
    if (!colon) return false;
    *out = atoi(colon + 1);
    return true;
}

static bool json_parse_ulong(const char *buf, const char *key, unsigned long *out)
{
    char needle[40];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *k = strstr(buf, needle);
    if (!k) return false;
    const char *colon = strchr(k, ':');
    if (!colon) return false;
    *out = strtoul(colon + 1, NULL, 10);
    return true;
}

// ---------------------------------------------------------------------------
// WS message handling
// ---------------------------------------------------------------------------

static void apply_config_from_json(const char *buf)
{
    xSemaphoreTake(s_cfg.mux, portMAX_DELAY);
    unsigned long ul_val;
    int int_val;
    if (json_parse_ulong(buf, "maxScore",         &ul_val))  s_cfg.cfg.maxScore         = ul_val;
    if (json_parse_ulong(buf, "minScore",         &ul_val))  s_cfg.cfg.minScore         = ul_val;
    if (json_parse_int  (buf, "defaultRollDelay", &int_val)) s_cfg.cfg.defaultRollDelay = int_val;
    if (json_parse_int  (buf, "rollDelayMod",     &int_val)) s_cfg.cfg.rollDelayMod     = int_val;
    if (json_parse_int  (buf, "rollThresh",       &int_val)) s_cfg.cfg.rollThresh       = int_val;
    if (json_parse_int  (buf, "slowRollThresh",   &int_val)) s_cfg.cfg.slowRollThresh   = int_val;
    if (json_parse_int  (buf, "slowRollDelayMod", &int_val)) s_cfg.cfg.slowRollDelayMod = int_val;
    if (json_parse_int  (buf, "defaultIncrement", &int_val)) s_cfg.cfg.defaultIncrement = int_val;
    if (json_parse_int  (buf, "blinkDelay",       &int_val)) s_cfg.cfg.blinkDelay       = int_val;
    if (json_parse_int  (buf, "waveDuration",     &int_val)) s_cfg.cfg.waveDuration     = int_val;
    if (json_parse_int  (buf, "waveDelay",        &int_val)) s_cfg.cfg.waveDelay        = int_val;
    s_cfg.dirty = true;
    xSemaphoreGive(s_cfg.mux);
    ESP_LOGI(TAG, "config received from backend");
}

static void on_ws_text(const char *data, size_t len)
{
    // Copy to NUL-terminated buffer (stack if small)
    char stackbuf[512];
    char *buf = stackbuf;
    if (len >= sizeof(stackbuf)) {
        buf = (char *)malloc(len + 1);
        if (!buf) return;
    }
    memcpy(buf, data, len);
    buf[len] = '\0';

    // Route by "type"
    const char *t = strstr(buf, "\"type\"");
    if (t) {
        const char *q1 = strchr(t + 6, '"');
        if (q1) {
            q1++;
            const char *q2 = strchr(q1, '"');
            if (q2) {
                int tl = q2 - q1;
                if (tl == 6 && strncmp(q1, "config", 6) == 0) {
                    apply_config_from_json(buf);
                } else if (tl == 4 && strncmp(q1, "ping", 4) == 0) {
                    // no-op; native WS keepalive is enough
                } else {
                    ESP_LOGW(TAG, "unknown msg type");
                }
            }
        }
    }

    if (buf != stackbuf) free(buf);
}

static void on_ws_connect(void)
{
    // Send hello. App version from esp_app_get_description().
    const esp_app_desc_t *d = esp_app_get_description();
    char msg[96];
    int n = snprintf(msg, sizeof(msg),
        "{\"type\":\"hello\",\"fw\":\"%s\"}", d ? d->version : "?");
    ws_client_send_text(msg, n);
    ESP_LOGI(TAG, "sent hello");
}

// ---------------------------------------------------------------------------
// PunchMeter task (core 1)
// ---------------------------------------------------------------------------

static void punchmeter_task(void *arg)
{
    punchmeter_setup();
    int prev_score = -1;

    while (1) {
        if (s_cfg.dirty && xSemaphoreTake(s_cfg.mux, 0) == pdTRUE) {
            PunchmeterConfig local = s_cfg.cfg;
            s_cfg.dirty = false;
            xSemaphoreGive(s_cfg.mux);
            punchmeter_set_config(&local);
        }

        punchmeter_loop();

        int cur = punchmeter_get_last_score();
        if (cur >= 0 && cur != prev_score) {
            score_item_t item = {
                .id = s_next_score_id++,
                .score = cur,
                .ts_epoch = time(NULL),
            };
            score_queue_push(&item);
            prev_score = cur;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// ---------------------------------------------------------------------------
// Uploader task — drain queue → WS
// ---------------------------------------------------------------------------

static void uploader_task(void *arg)
{
    while (1) {
        score_item_t item;
        if (score_queue_peek(&item)) {
            if (ws_client_is_connected()) {
                char msg[128];
                int n = snprintf(msg, sizeof(msg),
                    "{\"type\":\"score\",\"id\":%d,\"score\":%d,\"ts\":%lld}",
                    item.id, item.score, (long long)item.ts_epoch);
                if (ws_client_send_text(msg, n) == ESP_OK) {
                    score_queue_pop();
                } else {
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// ---------------------------------------------------------------------------
// app_main
// ---------------------------------------------------------------------------

extern "C" void app_main(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "Bapthit firmware v%s started (probe mode)",
             app_desc ? app_desc->version : "?");

    // OTA rollback confirmation
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK
        && ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "OTA firmware validated");
    }

    // NVS (required by WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Netif + event loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Init mDNS so that gethostbyname() can resolve <host>.local
    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set("bapthit-device");

    // Synchronization primitives
    s_cfg.mux = xSemaphoreCreateMutex();
    score_queue_init();
    punchmeter_set_logger(pm_log_sink);

    // PunchMeter task on core 1
    xTaskCreatePinnedToCore(punchmeter_task, "punchmeter", 4096, NULL, 5, NULL, 1);

    // WiFi STA — block until IP
    ESP_ERROR_CHECK(wifi_sta_start_and_wait());

    // WebSocket client
    ws_client_cfg_t wsc = {
        .host = CONFIG_BAPTHIT_BACKEND_HOST,
        .port = CONFIG_BAPTHIT_BACKEND_PORT,
        .path = "/ws/device",
        .on_connect = on_ws_connect,
        .on_text = on_ws_text,
    };
    ESP_ERROR_CHECK(ws_client_start(&wsc));

    // Uploader task
    xTaskCreate(uploader_task, "uploader", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "Running on partition: %s", running->label);
}
```

**Step 2: Mettre à jour `main/CMakeLists.txt` — REQUIRES finales**

Le build a besoin du component `mdns` dans REQUIRES (même si c'est managed, c'est via le nom espressif__mdns → on l'inclut via les component deps auto-générées). Vérifions : esp_event et esp_netif sont déjà là. Pour `mdns.h`, ajoute `mdns` à REQUIRES et `esp_timer` au besoin :

```cmake
idf_component_register(
    SRCS "main.cpp" "wifi_sta.c" "score_queue.c" "ws_client.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event nvs_flash esp_netif esp_app_format app_update
             lwip esp_timer idf_component
    PRIV_REQUIRES mdns esp_websocket_client
)

if(DEFINED ENV{BAPTHIT_DEV})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE BAPTHIT_DEV=1)
endif()
```

Note : `idf_component` reste car `punchmeter_api.h` est exposé par le component punchmeter.

**Step 3: Build**

Run : `idf.py build 2>&1 | tail -60`
Expected : build passes. Binary size significantly reduced vs legacy.

**Step 4: Commit**

```bash
git add main/main.cpp main/CMakeLists.txt
git commit -m "esp: rewrite main.cpp as pure probe (STA+mDNS+WS+score_queue)"
```

**Acceptance Criteria:**

- [ ] Run `idf.py build 2>&1 | grep "error:"` — Expected: no matches
- [ ] Run `wc -l main/main.cpp` — Expected: ~400 lines or less (down from 1253)
- [ ] Run `grep -c "esp_http_server\|wifi_init_ap\|captive" main/main.cpp` — Expected: `0`

---

### Task 20: Nettoyer les anciens legacy dans le repo

**Files:**
- Delete: (any remaining) old captive portal / HTTP artefacts
- Modify: `README.md` (if relevant)

**Step 1: Grep pour s'assurer qu'il ne reste rien de l'ancien monde**

Run :
```bash
grep -rn "192.168.4.1" --include="*.c*" --include="*.h" --include="*.py" --include="*.md" .
grep -rn "CAPTIVE\|captive_redirect\|dns_server_task" --include="*.c*" --include="*.h" .
grep -rn "laptop_ip\|fetch_history_from_laptop" --include="*.c*" --include="*.h" --include="*.py" --include="*.html" .
```
Expected : aucune occurrence fonctionnelle restante (ok si c'est dans `ai_docs/`).

**Step 2: Mise à jour README**

Si `README.md` mentionne `BAPTHIT` AP / 192.168.4.1 / captive portal, mettre à jour le paragraphe pour refléter :
- ESP en STA sur WiFi maison (creds dans `sdkconfig.defaults.local`)
- Backend tourne sur le portable avec `uvicorn server:app --host 0.0.0.0 --port 6969`
- UI accessible sur `http://bapthit-server.local:6969/`

(Lire `README.md` d'abord — si absent ou non pertinent, skipper cette étape.)

**Step 3: Commit si changements**

```bash
git add -u
git commit -m "docs: update README for probe+central architecture" || echo "no changes"
```

**Acceptance Criteria:**

- [ ] Run `grep -rn "192.168.4.1" --include="*.c*" --include="*.h" --include="*.py" .` — Expected: 0 matches (hors ai_docs/)
- [ ] Run `grep -rn "fetch_history_from_laptop" .` — Expected: 0 matches

---

### Task 21: Flash + monitor + premier tour WiFi→mDNS→WS

**Files:** (no edits, just verification)

**Step 1: Lancer le backend en foreground**

Dans un terminal :
```bash
cd backend && .venv/bin/uvicorn server:app --host 0.0.0.0 --port 6969
```

**Step 2: Flash l'ESP**

Dans un autre terminal :
```bash
source ~/.espressif/v6.0/esp-idf/export.sh
idf.py -p /dev/ttyUSB0 flash monitor
```

**Observations attendues dans le monitor :**
- `Bapthit firmware vX started (probe mode)`
- `wifi_sta: Waiting for IP on SSID '<yourssid>'...`
- `wifi_sta: Got IP <ip>`
- `ws_client: CONNECTED`
- `sent hello`
- `config received from backend`

**Observations attendues côté backend (uvicorn logs) :**
- `Device connected`
- `Device hello: fw=<version>`

**Si ça échoue :**
- WiFi : vérifier les creds dans `sdkconfig.defaults.local`, forcer `idf.py fullclean && idf.py build flash`.
- mDNS : `avahi-browse -art` pour confirmer la pub, `ping bapthit-server.local` depuis le portable. Si le ESP ne résout pas, `CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES=y` doit être actif (par défaut avec le component mdns).
- WS : vérifier que `CONFIG_BAPTHIT_BACKEND_PORT` est bien 6969.

**Step 3: Commit (no code change — just a marker)**

Si tout passe, skipper. Sinon, corriger et commit par-dessus les tâches précédentes.

**Acceptance Criteria:**

- [ ] Monitor affiche `ws_client: CONNECTED` dans les 15 secondes après `Got IP`
- [ ] Backend affiche `Device connected` + `Device hello: fw=...`

---

### Task 22: Validation bout-en-bout (plan de test du design)

**Files:** (no edits)

**Step 1: Score E2E**

Avec backend + ESP connectés, taper sur le PunchMeter (ou envoyer `SCORE:750` en série si on remet la fonction de test — hors scope). Vérifier :

```bash
sqlite3 backend/bapthit.db "SELECT * FROM scores ORDER BY id DESC LIMIT 5"
```
Expected : la dernière ligne correspond au coup tapé.

**Step 2: UI**

Dans Chrome (ou Chrome DevTools MCP) : `http://bapthit-server.local:6969/`

- La page charge.
- Le score du coup précédent apparaît dans la liste.
- Cliquer "Claim" → saisir un nom → Enregistrer. Vérifier en SQL : `SELECT name FROM scores WHERE id=<last>` renvoie le nom.
- Joindre une photo, recharger, voir la vignette dans la liste.

**Step 3: Config round-trip**

- Ouvrir settings dans l'UI, modifier `maxScore` à 12345, sauvegarder.
- Monitor ESP affiche `config received from backend`.
- Vérifier dans le PunchMeter que le comportement change (visuellement, seuil de score différent).

**Step 4: Offline/replay**

- Couper le backend (`Ctrl-C` sur uvicorn).
- Taper 3 coups de poing.
- Monitor ESP affiche `ws_client: DISCONNECTED` puis `ws_client: ERROR` en boucle.
- Relancer le backend.
- Monitor ESP affiche `ws_client: CONNECTED` + `sent hello`.
- Backend reçoit 3 messages de score.
- `sqlite3 backend/bapthit.db "SELECT * FROM scores ORDER BY id DESC LIMIT 3"` : les 3 coups présents.

**Step 5: Validation finale et PR**

Si tout est vert :

```bash
git log --oneline master..HEAD
git push -u origin feature/esp-probe-backend-central
gh pr create --title "ESP probe + backend central architecture" \
  --body "$(cat <<'EOF'
## Summary
- ESP32 passe de SoftAP+HTTP vers sonde STA+WebSocket pure
- Backend FastAPI devient source-of-truth (config, scores, photos, UI)
- mDNS discovery (zeroconf côté backend, lwip+mdns côté ESP)
- UI déplacée de main/index.html vers backend/static/index.html

## Test plan
- [x] WiFi STA + mDNS resolve
- [x] WS connect + hello + config push
- [x] Score E2E tap → DB
- [x] Claim name + photo upload
- [x] Config round-trip (UI → DB → WS → PunchMeter)
- [x] Offline buffer + replay on reconnect
EOF
)"
```

**Acceptance Criteria:**

- [ ] Score E2E : `sqlite3 backend/bapthit.db "SELECT COUNT(*) FROM scores"` augmente après chaque coup tapé
- [ ] Config round-trip : ESP monitor affiche `config received from backend` après un POST `/api/config`
- [ ] Offline replay : 3 coups tapés backend-down → tous présents après reconnect
- [ ] UI : chargement OK depuis `http://bapthit-server.local:6969/` sur un téléphone/autre device du LAN

---

## Notes pour l'exécutant

- **Pas de tests unitaires** : validation manuelle uniquement. Le design l'exige.
- **Ordre important** : ne pas tenter de paralléliser Phase 1 et Phase 3 — Task 12 (UI) peut tourner en parallèle de la Phase 3 ESP sans conflit.
- **Big-bang assumé** : entre Task 20 et Task 21, le firmware legacy est cassé — c'est OK, on ne supporte pas de mode hybride.
- **Si `esp_websocket_client` 1.4.0 n'existe pas** : `idf.py add-dependency "espressif/esp_websocket_client"` laissera CMake choisir la dernière compatible, ajuster la version dans `idf_component.yml`.
- **`time(NULL)` sur ESP** : sans SNTP, renvoie 0. C'est OK — le backend utilise `datetime.now()` en fallback quand `ts==0`. Si on veut une vraie heure, on pourra ajouter SNTP plus tard (out of scope).
- **PunchMeter component** : totalement intouché. On ne change que le code qui l'utilise.
