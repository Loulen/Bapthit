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
