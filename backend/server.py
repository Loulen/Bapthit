import sqlite3
import httpx
from pathlib import Path
from contextlib import asynccontextmanager

from fastapi import FastAPI, UploadFile, File, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse
from pydantic import BaseModel

ESP32_IP = "192.168.4.1"
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


async def register_with_esp32(my_ip: str):
    """Register this backend with the ESP32."""
    try:
        async with httpx.AsyncClient(timeout=5.0) as client:
            await client.post(
                f"http://{ESP32_IP}/api/backend",
                json={"ip": my_ip},
            )
            print(f"Registered with ESP32 as {my_ip}")
    except Exception as e:
        print(f"Failed to register with ESP32: {e}")
        print("Will retry when ESP32 is available.")


@asynccontextmanager
async def lifespan(app: FastAPI):
    init_db()
    PHOTOS_DIR.mkdir(exist_ok=True)

    # Detect our IP on the BAPTHIT network
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((ESP32_IP, 80))
        my_ip = s.getsockname()[0]
    except Exception:
        my_ip = "127.0.0.1"
    finally:
        s.close()

    await register_with_esp32(my_ip)
    yield


app = FastAPI(lifespan=lifespan)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)


# --- Models ---

class ScoreIn(BaseModel):
    id: int
    hour: int
    minute: int
    score: int


class ClaimIn(BaseModel):
    id: int
    name: str


# --- Endpoints ---

@app.post("/api/scores")
def receive_score(data: ScoreIn):
    """ESP32 pushes new scores here."""
    conn = get_db()
    conn.execute(
        "INSERT OR IGNORE INTO scores (id, hour, minute, score) VALUES (?, ?, ?, ?)",
        (data.id, data.hour, data.minute, data.score),
    )
    conn.commit()
    conn.close()
    return {"ok": True}


@app.post("/api/scores/claim")
def receive_claim(data: ClaimIn):
    """ESP32 forwards claims here."""
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
    """ESP32 fetches all persisted scores on boot."""
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
    """Phone uploads photo directly here."""
    # Save photo
    photo_path = PHOTOS_DIR / f"{score_id}.jpg"
    MAX_PHOTO_SIZE = 10 * 1024 * 1024  # 10 MB
    contents = await photo.read(MAX_PHOTO_SIZE + 1)
    if len(contents) > MAX_PHOTO_SIZE:
        raise HTTPException(status_code=413, detail="Photo too large (max 10 MB)")
    photo_path.write_bytes(contents)

    # Update DB
    conn = get_db()
    conn.execute("UPDATE scores SET has_photo = 1 WHERE id = ?", (score_id,))
    conn.commit()
    conn.close()

    # Notify ESP32
    try:
        async with httpx.AsyncClient(timeout=3.0) as client:
            await client.post(
                f"http://{ESP32_IP}/api/photo_ok",
                json={"id": score_id},
            )
    except Exception:
        pass  # ESP32 will pick it up on next history fetch

    return {"ok": True}


@app.get("/api/photos/{score_id}")
def get_photo(score_id: int):
    """Phone fetches photo for display."""
    photo_path = PHOTOS_DIR / f"{score_id}.jpg"
    if not photo_path.exists():
        raise HTTPException(status_code=404, detail="Photo not found")
    return FileResponse(photo_path, media_type="image/jpeg")


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000)
