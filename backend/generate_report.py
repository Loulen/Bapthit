#!/usr/bin/env python3
"""One-shot script: reads bapthit.db + photos, generates a self-contained HTML report."""

import base64
import io
import sqlite3
import statistics
from pathlib import Path

from PIL import Image, ImageOps

DB_PATH = Path(__file__).parent / "bapthit.db"
PHOTOS_DIR = Path(__file__).parent / "photos"
OUTPUT_PATH = Path(__file__).parent / "report.html"


# ---------------------------------------------------------------------------
# Data loading
# ---------------------------------------------------------------------------

def event_sort_hour(h: int) -> int:
    """Map hour to event-chronological order (17→22 then 01→03)."""
    return h if h >= 12 else h + 24


def load_scores():
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    rows = conn.execute(
        "SELECT id, hour, minute, score, name, has_photo FROM scores ORDER BY id"
    ).fetchall()
    conn.close()
    scores = [dict(r) for r in rows]

    # Detect session break: if hour jumps backwards to afternoon after early
    # morning scores, everything after that is a different day.
    cutoff = len(scores)
    for i in range(1, len(scores)):
        prev_eh = event_sort_hour(scores[i - 1]["hour"])
        curr_eh = event_sort_hour(scores[i]["hour"])
        # A jump back of 8+ hours means new session (e.g. 03h → 16h)
        if curr_eh < prev_eh - 8:
            cutoff = i
            break

    return scores[:cutoff]


THUMB_PX = 200       # avatars/vignettes (100px display at 2x)
GALLERY_PX = 600     # gallery photos (large, ~300px display at 2x)
PHOTO_QUALITY = 75


def _resize_photo(path: Path, max_px: int, square: bool = True) -> bytes | None:
    if not path.exists():
        return None
    img = Image.open(path)
    img = ImageOps.exif_transpose(img)
    if square:
        w, h = img.size
        side = min(w, h)
        left = (w - side) // 2
        top = (h - side) // 2
        img = img.crop((left, top, left + side, top + side))
        img = img.resize((max_px, max_px), Image.LANCZOS)
    else:
        img.thumbnail((max_px, max_px), Image.LANCZOS)
    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=PHOTO_QUALITY, optimize=True)
    return buf.getvalue()


def load_photo_b64(score_id: int) -> str | None:
    data = _resize_photo(PHOTOS_DIR / f"{score_id}.jpg", THUMB_PX, square=True)
    if not data:
        return None
    return "data:image/jpeg;base64," + base64.b64encode(data).decode()


def load_gallery_b64(score_id: int) -> str | None:
    data = _resize_photo(PHOTOS_DIR / f"{score_id}.jpg", GALLERY_PX, square=False)
    if not data:
        return None
    return "data:image/jpeg;base64," + base64.b64encode(data).decode()


# ---------------------------------------------------------------------------
# Stats computation
# ---------------------------------------------------------------------------

def compute_stats(all_scores: list[dict]):
    legit = [s for s in all_scores if s["score"] < 999]
    cheater = [s for s in all_scores if s["score"] == 999 and s["name"]]
    claimed_legit = [s for s in legit if s["name"]]
    values = [s["score"] for s in legit]

    # First / last by id order
    first = all_scores[0]
    last = all_scores[-1]

    # Podium: top 3 scores (prefer one with photo on tie)
    top3 = sorted(legit, key=lambda s: (-s["score"], -s["has_photo"]))[:3]

    # Duration & cadence
    first_min = event_sort_hour(first["hour"]) * 60 + first["minute"]
    last_min = event_sort_hour(last["hour"]) * 60 + last["minute"]
    duration_min = last_min - first_min
    duration_h = duration_min // 60
    duration_m = duration_min % 60
    cadence_sec = round(duration_min * 60 / len(all_scores)) if all_scores else 0

    # Central tendency
    mean = round(statistics.mean(values))
    median = round(statistics.median(values))
    stdev = round(statistics.stdev(values)) if len(values) > 1 else 0
    q1 = round(statistics.median([v for v in values if v <= median]))
    q3 = round(statistics.median([v for v in values if v >= median]))

    # Per-person
    people: dict[str, list[int]] = {}
    for s in claimed_legit:
        people.setdefault(s["name"], []).append(s["score"])

    per_person = []
    for name, scores in people.items():
        # Find best photo for this person
        person_scores = [s for s in claimed_legit if s["name"] == name]
        photo_score = next((s for s in person_scores if s["has_photo"]), None)
        per_person.append({
            "name": name,
            "count": len(scores),
            "avg": round(statistics.mean(scores)),
            "best": max(scores),
            "photo_id": photo_score["id"] if photo_score else None,
        })
    per_person.sort(key=lambda p: p["avg"], reverse=True)

    # Per-hour count + average score (sorted by event chronology)
    per_hour: dict[int, int] = {}
    per_hour_scores: dict[int, list[int]] = {}
    for s in legit:
        per_hour[s["hour"]] = per_hour.get(s["hour"], 0) + 1
        per_hour_scores.setdefault(s["hour"], []).append(s["score"])
    per_hour = dict(sorted(per_hour.items(), key=lambda kv: event_sort_hour(kv[0])))
    per_hour_avg = {h: round(statistics.mean(v)) for h, v in
                    sorted(per_hour_scores.items(), key=lambda kv: event_sort_hour(kv[0]))}

    # Peak hour
    peak_hour = max(per_hour, key=per_hour.get)

    # Claim rate
    claimed_count = len([s for s in all_scores if s["name"]])
    claim_pct = round(claimed_count / len(all_scores) * 100) if all_scores else 0

    # Histogram (100-point buckets)
    histogram: dict[int, int] = {}
    for v in values:
        bucket = (v // 100) * 100
        histogram[bucket] = histogram.get(bucket, 0) + 1

    return {
        "total": len(all_scores),
        "total_legit": len(legit),
        "first": first,
        "last": last,
        "duration_h": duration_h,
        "duration_m": duration_m,
        "cadence_sec": cadence_sec,
        "podium": top3,
        "mean": mean,
        "median": median,
        "stdev": stdev,
        "q1": q1,
        "q3": q3,
        "per_person": per_person,
        "per_hour": per_hour,
        "per_hour_avg": per_hour_avg,
        "peak_hour": peak_hour,
        "claimed_count": claimed_count,
        "claim_pct": claim_pct,
        "histogram": dict(sorted(histogram.items())),
        "cheater_scores": cheater,
        "photo_scores": [s for s in all_scores if s["has_photo"]],
    }


# ---------------------------------------------------------------------------
# HTML generation
# ---------------------------------------------------------------------------

def fmt_time(s):
    return f"{s['hour']:02d}h{s['minute']:02d}"


def photo_img(score_id, css_class="photo"):
    b64 = load_photo_b64(score_id)
    if not b64:
        return ""
    return f'<img class="{css_class}" src="{b64}">'


def esc(text):
    return str(text).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;")


def generate_html(stats):
    s = stats

    # --- Build sections ---

    # Hero
    hero = f"""
    <div class="hero">
      <h1>BAPT'HIT</h1>
      <div class="hero-sub">Bilan de la soiree</div>
      <div class="hero-line"></div>
      <div class="hero-stats">
        <div class="hero-stat">
          <div class="hero-stat-val">{s['total']}</div>
          <div class="hero-stat-lbl">coups</div>
        </div>
        <div class="hero-stat">
          <div class="hero-stat-val">{s['duration_h']}h{s['duration_m']:02d}</div>
          <div class="hero-stat-lbl">de soiree</div>
        </div>
        <div class="hero-stat">
          <div class="hero-stat-val">~{s['cadence_sec']}s</div>
          <div class="hero-stat-lbl">entre chaque coup</div>
        </div>
      </div>
    </div>"""

    # First & Last
    def punch_card(score, label):
        name = esc(score["name"]) if score["name"] else "Anonyme"
        photo = photo_img(score["id"], "card-photo") if score["has_photo"] else ""
        return f"""
        <div class="punch-card">
          <div class="punch-label">{label}</div>
          {photo}
          <div class="punch-name">{name}</div>
          <div class="punch-score">{score['score']}<span class="punch-unit">pts</span></div>
          <div class="punch-time">{fmt_time(score)}</div>
        </div>"""

    first_last = f"""
    <div class="section">
      <h2>Premier &amp; Dernier Coup</h2>
      <div class="two-cards">
        {punch_card(s['first'], 'Premier coup')}
        {punch_card(s['last'], 'Dernier coup')}
      </div>
    </div>"""

    # Podium — top 3 scores, displayed as #2 | #1 | #3
    podium = s["podium"]  # already sorted 1st, 2nd, 3rd
    podium_order = [podium[1], podium[0], podium[2]]  # display: silver, gold, bronze
    podium_meta = [
        {"rank": 2, "cls": "silver", "height": "65%"},
        {"rank": 1, "cls": "gold",   "height": "100%"},
        {"rank": 3, "cls": "bronze", "height": "45%"},
    ]

    podium_cols = ""
    for score, meta in zip(podium_order, podium_meta):
        name = esc(score["name"]) if score["name"] else "Anonyme"
        photo = photo_img(score["id"], "podium-photo") if score["has_photo"] else ""
        podium_cols += f"""
        <div class="podium-col">
          <div class="podium-top">
            {photo}
            <div class="podium-name">{name}</div>
            <div class="podium-score">{score['score']}<span class="podium-unit">pts</span></div>
            <div class="podium-time">{fmt_time(score)}</div>
          </div>
          <div class="podium-bar {meta['cls']}" style="height:{meta['height']}">
            <span class="podium-rank">#{meta['rank']}</span>
          </div>
        </div>"""

    records = f"""
    <div class="section">
      <h2>Podium</h2>
      <div class="podium">{podium_cols}</div>
    </div>"""

    # Julia special mention
    julia_photo = ""
    julia_lines = []
    for cs in s["cheater_scores"]:
        if cs["has_photo"]:
            julia_photo = photo_img(cs["id"], "julia-photo")
        julia_lines.append(f'{fmt_time(cs)} &mdash; {cs["score"]} pts')
    julia_detail = "<br>".join(julia_lines)

    julia = f"""
    <div class="section julia-section">
      <h2>Mention Speciale</h2>
      <div class="julia-card">
        {julia_photo}
        <div class="julia-text">
          <div class="julia-name">Julia, la tricheuse assumee</div>
          <div class="julia-detail">{julia_detail}</div>
          <div class="julia-note">Score parfait de 999 &mdash; on salue l'audace.</div>
        </div>
      </div>
    </div>"""

    # Champions table
    medals = ["gold", "silver", "bronze"]
    rows_html = ""
    for i, p in enumerate(s["per_person"]):
        medal_cls = medals[i] if i < 3 else ""
        rank = f'<span class="medal {medal_cls}">#{i+1}</span>' if i < 3 else f"#{i+1}"
        avatar = ""
        if p["photo_id"]:
            avatar = photo_img(p["photo_id"], "champ-avatar")
        rows_html += f"""
        <tr class="{medal_cls}">
          <td class="rank-cell">{rank}</td>
          <td class="avatar-cell">{avatar}</td>
          <td>{esc(p['name'])}</td>
          <td class="num">{p['count']}</td>
          <td class="num avg-cell">{p['avg']}</td>
          <td class="num">{p['best']}</td>
        </tr>"""

    champions = f"""
    <div class="section">
      <h2>Classement des Champions</h2>
      <table class="champ-table">
        <thead>
          <tr><th></th><th></th><th>Nom</th><th>Coups</th><th>Moyenne</th><th>Meilleur</th></tr>
        </thead>
        <tbody>{rows_html}</tbody>
      </table>
    </div>"""

    # Stats globales
    stat_cards = f"""
    <div class="stat-grid">
      <div class="stat-card"><div class="stat-val">{s['mean']}</div><div class="stat-lbl">Moyenne</div></div>
      <div class="stat-card"><div class="stat-val">{s['median']}</div><div class="stat-lbl">Mediane</div></div>
      <div class="stat-card"><div class="stat-val">{s['q1']}</div><div class="stat-lbl">Q1 (25%)</div></div>
      <div class="stat-card"><div class="stat-val">{s['q3']}</div><div class="stat-lbl">Q3 (75%)</div></div>
      <div class="stat-card"><div class="stat-val">{s['stdev']}</div><div class="stat-lbl">Ecart-type</div></div>
      <div class="stat-card"><div class="stat-val">{s['claim_pct']}%</div><div class="stat-lbl">Claims ({s['claimed_count']}/{s['total']})</div></div>
    </div>"""

    # Peak hour callout
    peak_h = s["peak_hour"]
    peak_cnt = s["per_hour"][peak_h]
    peak_avg = s["per_hour_avg"][peak_h]
    peak_callout = f"""
    <div class="peak-callout">
      <span class="peak-icon">&#x1f525;</span>
      <div class="peak-text">
        <strong>Heure la plus chaude : {peak_h:02d}h</strong> &mdash;
        {peak_cnt} coups, score moyen {peak_avg} pts
      </div>
    </div>"""

    # Per-hour bars (count)
    max_hour_count = max(s["per_hour"].values()) if s["per_hour"] else 1
    hour_bars = ""
    for h, cnt in s["per_hour"].items():
        pct = round(cnt / max_hour_count * 100)
        hour_bars += f"""
        <div class="hour-row">
          <span class="hour-label">{h:02d}h</span>
          <div class="hour-bar-bg"><div class="hour-bar" style="width:{pct}%"></div></div>
          <span class="hour-count">{cnt}</span>
        </div>"""

    per_hour_count = f"""
    <div class="hour-chart">
      <div class="hour-title">Nombre de coups par heure</div>
      {hour_bars}
    </div>"""

    # Per-hour average score curve (CSS-only mini line chart)
    avg_values = list(s["per_hour_avg"].values())
    avg_min = min(avg_values) - 50
    avg_max = max(avg_values) + 50
    avg_range = avg_max - avg_min or 1

    avg_points = []
    avg_labels_html = ""
    n = len(avg_values)
    hours_list = list(s["per_hour_avg"].keys())
    for i, (h, avg) in enumerate(s["per_hour_avg"].items()):
        x_pct = (i / (n - 1)) * 100 if n > 1 else 50
        y_pct = 100 - ((avg - avg_min) / avg_range) * 100
        avg_points.append(f"{x_pct:.1f}% {y_pct:.1f}%")
        avg_labels_html += f"""
        <div class="avg-point" style="left:{x_pct:.1f}%;top:{y_pct:.1f}%">
          <div class="avg-dot"></div>
          <div class="avg-val">{avg}</div>
        </div>"""

    polygon_points = ", ".join(avg_points)
    # Build SVG polyline for the curve
    svg_points = " ".join(
        f"{(i / (n - 1)) * 100 if n > 1 else 50},{100 - ((avg - avg_min) / avg_range) * 100}"
        for i, avg in enumerate(avg_values)
    )

    avg_x_labels = ""
    for i, h in enumerate(hours_list):
        x_pct = (i / (n - 1)) * 100 if n > 1 else 50
        avg_x_labels += f'<span class="avg-x-label" style="left:{x_pct:.1f}%">{h:02d}h</span>'

    per_hour_avg_section = f"""
    <div class="hour-chart avg-chart">
      <div class="hour-title">Score moyen par heure &mdash; la courbe de la soiree</div>
      <div class="avg-line-container">
        <svg class="avg-svg" viewBox="0 0 100 100" preserveAspectRatio="none">
          <polyline points="{svg_points}" fill="none" stroke="#e81a1a" stroke-width="2"
                    vector-effect="non-scaling-stroke" stroke-linejoin="round"/>
        </svg>
        {avg_labels_html}
      </div>
      <div class="avg-x-axis">{avg_x_labels}</div>
    </div>"""

    global_stats = f"""
    <div class="section">
      <h2>Statistiques Globales</h2>
      <div class="stats-note">{s['total_legit']} scores (hors triche)</div>
      {stat_cards}
      {peak_callout}
      {per_hour_count}
      {per_hour_avg_section}
    </div>"""

    # Histogram
    max_bucket_count = max(s["histogram"].values()) if s["histogram"] else 1
    hist_bars = ""
    for bucket, cnt in s["histogram"].items():
        pct = round(cnt / max_bucket_count * 100)
        label = f"{bucket}" if bucket < 900 else "900+"
        hist_bars += f"""
        <div class="hist-col">
          <div class="hist-count">{cnt}</div>
          <div class="hist-bar" style="height:{pct}%"></div>
          <div class="hist-label">{label}</div>
        </div>"""

    histogram = f"""
    <div class="section">
      <h2>Distribution des Scores</h2>
      <div class="histogram">{hist_bars}</div>
    </div>"""

    # Photo gallery
    gallery_items = ""
    for ps in s["photo_scores"]:
        b64 = load_gallery_b64(ps["id"])
        if not b64:
            continue
        name = esc(ps["name"]) if ps["name"] else "Anonyme"
        gallery_items += f"""
        <div class="gallery-item">
          <img class="gallery-img" src="{b64}">
          <div class="gallery-caption">
            <span class="gallery-name">{name}</span>
            <span class="gallery-score">{ps['score']} pts</span>
            <span class="gallery-time">{fmt_time(ps)}</span>
          </div>
        </div>"""

    gallery = f"""
    <div class="section">
      <h2>Photos</h2>
      <div class="gallery">{gallery_items}</div>
    </div>"""

    # --- Assemble ---
    html = f"""<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>BAPT'HIT — Bilan</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link href="https://fonts.googleapis.com/css2?family=Bebas+Neue&family=Source+Sans+3:wght@400;600;700&display=swap" rel="stylesheet">
<style>
*, *::before, *::after {{ margin: 0; padding: 0; box-sizing: border-box; }}

:root {{
  --bg: #0a0a0a;
  --surface: #141414;
  --surface-alt: #1a1a1a;
  --border: #2a2a2a;
  --red: #e81a1a;
  --red-glow: rgba(232,26,26,0.25);
  --red-dim: #8b1111;
  --gold: #d4a520;
  --gold-dim: #8b6914;
  --silver: #a0a0a0;
  --bronze: #cd7f32;
  --text: #e8e8e8;
  --text-dim: #999;
  --text-muted: #555;
}}

html {{
  print-color-adjust: exact;
  -webkit-print-color-adjust: exact;
}}

body {{
  background: var(--bg);
  color: var(--text);
  font-family: 'Source Sans 3', 'Segoe UI', sans-serif;
  font-size: 15px;
  line-height: 1.5;
  -webkit-font-smoothing: antialiased;
}}

.page {{
  max-width: 800px;
  margin: 0 auto;
  padding: 40px 32px 60px;
}}

/* ---- HERO ---- */

.hero {{
  text-align: center;
  padding: 48px 0 32px;
}}

.hero h1 {{
  font-family: 'Bebas Neue', sans-serif;
  font-size: 5.5rem;
  letter-spacing: 0.15em;
  color: var(--red);
  line-height: 1;
  text-shadow: 0 0 40px var(--red-glow), 0 0 80px rgba(232,26,26,0.1);
}}

.hero-sub {{
  font-size: 1.1rem;
  color: var(--text-dim);
  margin-top: 8px;
  letter-spacing: 0.04em;
}}

.hero-line {{
  width: 120px;
  height: 3px;
  background: var(--red);
  margin: 20px auto 0;
  box-shadow: 0 0 12px var(--red-glow);
}}

.hero-stats {{
  display: flex;
  justify-content: center;
  gap: 40px;
  margin-top: 28px;
}}

.hero-stat {{
  text-align: center;
}}

.hero-stat-val {{
  font-family: 'Bebas Neue', sans-serif;
  font-size: 2.4rem;
  color: var(--text);
  line-height: 1;
}}

.hero-stat-lbl {{
  font-size: 0.8rem;
  color: var(--text-muted);
  text-transform: uppercase;
  letter-spacing: 0.08em;
  margin-top: 4px;
}}

/* ---- SECTIONS ---- */

.section {{
  margin-top: 48px;
  page-break-inside: avoid;
}}

.section h2 {{
  font-family: 'Bebas Neue', sans-serif;
  font-size: 2rem;
  letter-spacing: 0.1em;
  color: var(--text);
  border-bottom: 2px solid var(--border);
  padding-bottom: 8px;
  margin-bottom: 20px;
}}

/* ---- PUNCH CARDS (first/last, records) ---- */

.two-cards {{
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 16px;
}}

.punch-card {{
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: 10px;
  padding: 24px 20px;
  text-align: center;
}}

.punch-label {{
  font-family: 'Bebas Neue', sans-serif;
  font-size: 1rem;
  letter-spacing: 0.12em;
  color: var(--text-muted);
  text-transform: uppercase;
  margin-bottom: 12px;
}}

.card-photo {{
  width: 80px;
  height: 80px;
  border-radius: 50%;
  object-fit: cover;
  border: 3px solid var(--border);
  margin-bottom: 12px;
}}

.punch-name {{
  font-size: 1.1rem;
  font-weight: 700;
  color: var(--text);
}}

.punch-score {{
  font-family: 'Bebas Neue', sans-serif;
  font-size: 2.8rem;
  color: var(--red);
  line-height: 1.1;
  margin-top: 4px;
}}

.punch-unit {{
  font-size: 1rem;
  color: var(--text-dim);
  margin-left: 4px;
}}

.punch-time {{
  font-size: 0.9rem;
  color: var(--text-muted);
  margin-top: 4px;
}}

/* ---- PODIUM ---- */

.podium {{
  display: flex;
  align-items: flex-end;
  justify-content: center;
  gap: 10px;
  padding-top: 16px;
}}

.podium-col {{
  flex: 1;
  max-width: 200px;
  display: flex;
  flex-direction: column;
  align-items: center;
}}

.podium-top {{
  text-align: center;
  margin-bottom: 12px;
}}

.podium-photo {{
  width: 72px;
  height: 72px;
  border-radius: 50%;
  object-fit: cover;
  border: 3px solid var(--border);
  margin-bottom: 8px;
}}

.podium-col:nth-child(2) .podium-photo {{
  width: 90px;
  height: 90px;
  border-color: var(--gold-dim);
}}

.podium-name {{
  font-size: 0.95rem;
  font-weight: 700;
  color: var(--text);
}}

.podium-score {{
  font-family: 'Bebas Neue', sans-serif;
  font-size: 2rem;
  line-height: 1.1;
  color: var(--text-dim);
}}

.podium-col:nth-child(2) .podium-score {{
  font-size: 2.6rem;
  color: var(--gold);
  text-shadow: 0 0 20px rgba(212,165,32,0.3);
}}

.podium-unit {{
  font-size: 0.85rem;
  color: var(--text-muted);
  margin-left: 2px;
}}

.podium-time {{
  font-size: 0.8rem;
  color: var(--text-muted);
  margin-top: 2px;
}}

.podium-bar {{
  width: 100%;
  border-radius: 6px 6px 0 0;
  display: flex;
  align-items: flex-start;
  justify-content: center;
  padding-top: 12px;
  min-height: 40px;
}}

.podium-bar.gold {{
  background: linear-gradient(180deg, rgba(212,165,32,0.35) 0%, rgba(212,165,32,0.08) 100%);
  border: 1px solid var(--gold-dim);
  border-bottom: none;
}}

.podium-bar.silver {{
  background: linear-gradient(180deg, rgba(160,160,160,0.2) 0%, rgba(160,160,160,0.05) 100%);
  border: 1px solid rgba(160,160,160,0.3);
  border-bottom: none;
}}

.podium-bar.bronze {{
  background: linear-gradient(180deg, rgba(205,127,50,0.2) 0%, rgba(205,127,50,0.05) 100%);
  border: 1px solid rgba(205,127,50,0.3);
  border-bottom: none;
}}

.podium-rank {{
  font-family: 'Bebas Neue', sans-serif;
  font-size: 1.6rem;
  letter-spacing: 0.05em;
}}

.gold .podium-rank {{ color: var(--gold); }}
.silver .podium-rank {{ color: var(--silver); }}
.bronze .podium-rank {{ color: var(--bronze); }}

/* ---- JULIA ---- */

.julia-section h2 {{
  color: var(--red);
}}

.julia-card {{
  background: var(--surface);
  border: 2px dashed var(--red-dim);
  border-radius: 10px;
  padding: 24px;
  display: flex;
  gap: 24px;
  align-items: center;
}}

.julia-photo {{
  width: 100px;
  height: 100px;
  border-radius: 50%;
  object-fit: cover;
  border: 3px solid var(--red-dim);
  flex-shrink: 0;
}}

.julia-name {{
  font-family: 'Bebas Neue', sans-serif;
  font-size: 1.5rem;
  letter-spacing: 0.06em;
  color: var(--red);
}}

.julia-detail {{
  color: var(--text-dim);
  margin-top: 6px;
  font-size: 0.95rem;
}}

.julia-note {{
  margin-top: 10px;
  font-style: italic;
  color: var(--text-muted);
  font-size: 0.9rem;
}}

/* ---- CHAMPIONS TABLE ---- */

.champ-table {{
  width: 100%;
  border-collapse: collapse;
}}

.champ-table th {{
  font-family: 'Bebas Neue', sans-serif;
  font-size: 0.9rem;
  letter-spacing: 0.1em;
  color: var(--text-muted);
  text-transform: uppercase;
  text-align: left;
  padding: 8px 10px;
  border-bottom: 2px solid var(--border);
}}

.champ-table td {{
  padding: 10px 10px;
  border-bottom: 1px solid var(--border);
  font-size: 0.95rem;
}}

.champ-table .num {{
  text-align: right;
  font-variant-numeric: tabular-nums;
}}

.champ-table .avg-cell {{
  font-weight: 700;
  color: var(--red);
}}

.rank-cell {{
  width: 40px;
  text-align: center !important;
}}

.avatar-cell {{
  width: 36px;
  padding: 4px !important;
}}

.champ-avatar {{
  width: 32px;
  height: 32px;
  border-radius: 50%;
  object-fit: cover;
  display: block;
}}

.medal {{
  font-family: 'Bebas Neue', sans-serif;
  font-size: 1.1rem;
}}

.medal.gold {{ color: var(--gold); }}
.medal.silver {{ color: var(--silver); }}
.medal.bronze {{ color: var(--bronze); }}

tr.gold td {{ background: rgba(212,165,32,0.06); }}
tr.silver td {{ background: rgba(160,160,160,0.04); }}
tr.bronze td {{ background: rgba(205,127,50,0.04); }}

/* ---- STATS ---- */

.stats-note {{
  color: var(--text-muted);
  font-size: 0.85rem;
  margin-bottom: 16px;
}}

.stat-grid {{
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  gap: 12px;
  margin-bottom: 20px;
}}

.stat-card {{
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: 18px 14px;
  text-align: center;
}}

.stat-val {{
  font-family: 'Bebas Neue', sans-serif;
  font-size: 2.2rem;
  color: var(--red);
  line-height: 1;
}}

.stat-lbl {{
  font-size: 0.8rem;
  color: var(--text-muted);
  text-transform: uppercase;
  letter-spacing: 0.08em;
  margin-top: 6px;
}}

/* Per-hour bars */

.hour-chart {{
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: 8px;
  padding: 20px;
}}

.hour-title {{
  font-family: 'Bebas Neue', sans-serif;
  font-size: 1.1rem;
  letter-spacing: 0.08em;
  color: var(--text-dim);
  margin-bottom: 12px;
}}

.hour-row {{
  display: flex;
  align-items: center;
  gap: 10px;
  margin-bottom: 6px;
}}

.hour-label {{
  width: 32px;
  font-size: 0.85rem;
  color: var(--text-dim);
  text-align: right;
  font-variant-numeric: tabular-nums;
}}

.hour-bar-bg {{
  flex: 1;
  height: 16px;
  background: var(--surface-alt);
  border-radius: 3px;
  overflow: hidden;
}}

.hour-bar {{
  height: 100%;
  background: var(--red);
  border-radius: 3px;
  min-width: 2px;
}}

.hour-count {{
  width: 32px;
  font-size: 0.85rem;
  color: var(--text-dim);
  font-variant-numeric: tabular-nums;
}}

/* Peak hour callout */

.peak-callout {{
  display: flex;
  align-items: center;
  gap: 12px;
  background: var(--surface);
  border: 1px solid var(--red-dim);
  border-radius: 8px;
  padding: 14px 18px;
  margin-bottom: 20px;
}}

.peak-icon {{
  font-size: 1.5rem;
  line-height: 1;
}}

.peak-text {{
  font-size: 0.95rem;
  color: var(--text-dim);
}}

.peak-text strong {{
  color: var(--red);
}}

/* Average score curve */

.avg-chart {{
  margin-top: 16px;
}}

.avg-line-container {{
  position: relative;
  height: 140px;
  margin: 0 20px;
}}

.avg-svg {{
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
}}

.avg-point {{
  position: absolute;
  transform: translate(-50%, -50%);
  display: flex;
  flex-direction: column;
  align-items: center;
}}

.avg-dot {{
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: var(--red);
  border: 2px solid var(--bg);
}}

.avg-val {{
  font-size: 0.7rem;
  color: var(--text-dim);
  margin-top: 4px;
  font-variant-numeric: tabular-nums;
}}

.avg-x-axis {{
  position: relative;
  height: 20px;
  margin: 8px 20px 0;
}}

.avg-x-label {{
  position: absolute;
  transform: translateX(-50%);
  font-size: 0.75rem;
  color: var(--text-muted);
}}

/* ---- HISTOGRAM ---- */

.histogram {{
  display: flex;
  align-items: flex-end;
  gap: 6px;
  height: 200px;
  padding: 0 4px;
  border-bottom: 2px solid var(--border);
}}

.hist-col {{
  flex: 1;
  display: flex;
  flex-direction: column;
  align-items: center;
  height: 100%;
  justify-content: flex-end;
}}

.hist-count {{
  font-size: 0.75rem;
  color: var(--text-dim);
  margin-bottom: 4px;
  font-variant-numeric: tabular-nums;
}}

.hist-bar {{
  width: 100%;
  background: var(--red);
  border-radius: 3px 3px 0 0;
  min-height: 2px;
}}

.hist-label {{
  font-size: 0.7rem;
  color: var(--text-muted);
  margin-top: 8px;
  text-align: center;
}}

/* ---- GALLERY ---- */

.gallery {{
  display: grid;
  grid-template-columns: repeat(2, 1fr);
  gap: 16px;
}}

.gallery-item {{
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: 10px;
  overflow: hidden;
  page-break-inside: avoid;
}}

.gallery-img {{
  width: 100%;
  display: block;
  object-fit: cover;
  aspect-ratio: 4/3;
}}

.gallery-caption {{
  padding: 10px 14px;
  display: flex;
  align-items: baseline;
  gap: 8px;
}}

.gallery-name {{
  font-weight: 700;
  font-size: 0.95rem;
}}

.gallery-score {{
  font-family: 'Bebas Neue', sans-serif;
  font-size: 1.2rem;
  color: var(--red);
  margin-left: auto;
}}

.gallery-time {{
  font-size: 0.8rem;
  color: var(--text-muted);
}}

/* ---- PRINT ---- */

@media print {{
  body {{ background: var(--bg) !important; }}
  .page {{ padding: 20px 24px; }}
  .section {{ page-break-inside: avoid; }}
  .hero {{ padding: 24px 0 16px; }}
  .hero h1 {{ font-size: 4rem; }}
}}
</style>
</head>
<body>
<div class="page">
{hero}
{first_last}
{records}
{julia}
{champions}
{global_stats}
{histogram}
{gallery}
</div>
</body>
</html>"""

    return html


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    scores = load_scores()
    if not scores:
        print("No scores found in database.")
        return

    stats = compute_stats(scores)
    html = generate_html(stats)
    OUTPUT_PATH.write_text(html, encoding="utf-8")
    print(f"Report generated: {OUTPUT_PATH}")
    print(f"  {stats['total']} total scores, {stats['total_legit']} legit")
    print(f"  {len(stats['per_person'])} named players")
    print(f"  {len(stats['cheater_scores'])} cheater scores (Julia)")


if __name__ == "__main__":
    main()
