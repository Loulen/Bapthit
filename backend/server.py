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
