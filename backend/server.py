import asyncio
import json
import socket as _socket
import sqlite3
from collections import deque
from datetime import datetime
from pathlib import Path
from contextlib import asynccontextmanager

from fastapi import FastAPI, UploadFile, File, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel
from zeroconf import ServiceInfo
from zeroconf.asyncio import AsyncZeroconf

PHOTOS_DIR = Path(__file__).parent / "photos"
DB_PATH = Path(__file__).parent / "bapthit.db"


# Default PunchMeter config — must mirror the struct in main/main.cpp
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

# In-memory log buffer fed from the device over WS
LOG_BUFFER_MAX = 500
log_buffer: deque = deque(maxlen=LOG_BUFFER_MAX)
log_seq = 0


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
    conn.execute("""
        CREATE TABLE IF NOT EXISTS config (
            key TEXT PRIMARY KEY,
            value INTEGER NOT NULL
        )
    """)
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
        for k, v in DEFAULT_CONFIG.items():
            cfg.setdefault(k, v)
        return cfg
    except Exception as e:
        print(f"Config load failed, using defaults: {e}")
        return dict(DEFAULT_CONFIG)


def insert_score(id_: int, score: int, ts_epoch: int) -> None:
    dt = datetime.fromtimestamp(ts_epoch) if ts_epoch > 0 else datetime.now()
    conn = get_db()
    conn.execute(
        "INSERT OR IGNORE INTO scores (id, hour, minute, score) VALUES (?, ?, ?, ?)",
        (id_, dt.hour, dt.minute, score),
    )
    conn.commit()
    conn.close()


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
            mtype = msg.get("type")
            if mtype == "hello":
                fw = msg.get("fw", "?")
                print(f"Device hello: fw={fw}")
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
    except WebSocketDisconnect:
        print("Device disconnected")
    finally:
        await device_manager.detach(ws)


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
    if device_manager.is_connected():
        await device_manager.send_json({"type": "config", **cfg})
    return cfg


@app.get("/api/logs")
def get_logs(since: int = 0):
    return {"logs": [e for e in log_buffer if e["s"] > since]}


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


STATIC_DIR = Path(__file__).parent / "static"


@app.get("/")
def root():
    index = STATIC_DIR / "index.html"
    if not index.exists():
        raise HTTPException(status_code=404, detail="UI not built")
    return FileResponse(index, media_type="text/html")


app.mount("/static", StaticFiles(directory=str(STATIC_DIR)), name="static")


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=6969)
