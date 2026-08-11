# BAPT'HIT Stats Export — Design Document

## Overview

One-shot Python script that reads `bapthit.db` + photo files, generates a self-contained HTML report (photos as base64 inline). User prints to PDF from browser.

## Approach

- **Approach A**: Pure Python with f-string templates, CSS-only histograms, zero extra dependencies
- Script location: `backend/generate_report.py`
- Output: `backend/report.html` (self-contained, openable in any browser)

## Aesthetic Direction

**"Fight Night Recap"** — dark editorial, boxing poster meets infographic.

- **Fonts**: Bebas Neue (display/titles) + Source Sans 3 (body) via Google Fonts `@import`
- **Colors**: Dark background (#0a0a0a), red accent (#e81a1a), gold for champions (#d4a520)
- **Print**: `print-color-adjust: exact` to preserve dark theme

## Report Sections (top to bottom)

### 1. Hero Header
- "BAPT'HIT" massive title, event date, total punch count

### 2. Premier & Dernier Coup
- Two side-by-side cards: time, score, name, photo if available
- "Opening round / Closing round" styling

### 3. Records (excluding score=999)
- Min score + Max score with photos if they exist
- Max highlighted in gold

### 4. Mention Speciale: Julia
- Fun callout for her 2x 999 cheating scores
- Her photo from score id=144

### 5. Classement des Champions
- Per-person average table (claimed scores, excluding 999)
- Photo avatars where available
- Top 3 with gold/silver/bronze

### 6. Stats Globales
- Cards: mean, median, Q1, Q3
- Total scores + per-hour breakdown (horizontal bars)

### 7. Histogramme
- Vertical CSS bars per 100-point bucket (0-99 through 900-999)
- Count labels on each bar

## Data Rules

- Scores = 999 excluded from all stats except Julia's special section
- "Claimed" = `name != ""`
- Per-person stats only use claimed scores (excluding 999)
- Global stats (mean, median, quartiles, histogram) use ALL scores excluding 999
- Bato/Batoo likely same person but kept separate (data as-is)
