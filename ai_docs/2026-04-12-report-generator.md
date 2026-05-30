# BAPT'HIT Report Generator — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** One-shot Python script that reads bapthit.db + photos and generates a self-contained HTML stats report.

**Architecture:** Single `backend/generate_report.py` script. Reads SQLite directly, encodes photos as base64 data URIs, builds HTML via f-strings, writes `backend/report.html`. Zero new dependencies.

**Tech Stack:** Python 3, sqlite3, base64, pathlib (all stdlib)

---

### Task 1: Script skeleton — read DB, compute all stats, write empty HTML shell

**Files:**
- Create: `backend/generate_report.py`

**Step 1: Create the script with data loading and stats computation**

The script should:
1. Connect to `bapthit.db`, load all scores
2. Separate cheater scores (score == 999) from legitimate scores
3. Compute:
   - First and last score (by id order)
   - Min/max score (excluding 999) with their full row data
   - Global stats: mean, median, Q1, Q3 (excluding 999)
   - Per-person averages (claimed + non-999), sorted desc
   - Per-hour counts
   - Histogram buckets (100-point ranges, excluding 999)
   - Julia's cheating scores (score == 999 AND name != "")
4. Load photos as base64 data URIs for scores that have `has_photo=1`
5. Write a minimal HTML file with just a `<pre>` dump of the computed stats (we'll replace with real HTML in Task 2)

**Step 2: Run the script**

```bash
cd backend && python3 generate_report.py
```

Expected: `report.html` created, opening it shows a text dump of stats.

**Step 3: Commit**

```bash
git add backend/generate_report.py
git commit -m "feat(report): script skeleton — reads DB, computes stats"
```

**Acceptance Criteria:**
- [ ] Run `python3 backend/generate_report.py` — no errors, `backend/report.html` is created
- [ ] Open `backend/report.html` in browser — shows computed stats (first/last score, min/max, mean, median, quartiles, per-person, per-hour, histogram buckets)
- [ ] Photos are loaded as base64 strings (check script output or add a debug print for photo count)

---

### Task 2: Full HTML report with Fight Night design

**Files:**
- Modify: `backend/generate_report.py`

**Step 1: Replace the HTML template with the full report**

Build the complete HTML with these sections:

**Section 1 — Hero Header:**
- "BAPT'HIT" in Bebas Neue, massive
- Subtitle with date and total count
- Red decorative line

**Section 2 — Premier & Dernier Coup:**
- Two cards side by side
- Each shows: time (HHhMM), score, name (or "Anonyme"), photo if has_photo
- Left card = first score (lowest id), Right = last score (highest id)

**Section 3 — Records:**
- Two cards: Score minimum / Score maximum (excluding 999)
- Gold accent on max, photo if available
- If multiple scores share min/max, pick the one with a photo, else first occurrence

**Section 4 — Mention Spéciale Julia:**
- Her photo (score id=144)
- Text about her 2 scores at 999
- Fun/humorous tone, distinct visual treatment (e.g. caution-tape border or italic aside)

**Section 5 — Classement des Champions:**
- Table of per-person stats: rank, photo avatar, name, count, average, best
- Sorted by average desc
- Top 3 with gold/silver/bronze styling
- Only claimed scores, excluding 999

**Section 6 — Stats Globales:**
- Grid of stat cards: Moyenne, Médiane, Q1, Q3
- Total scores count
- Per-hour horizontal bar chart (CSS only: `<div>` bars with width%)

**Section 7 — Histogramme:**
- Vertical CSS bar chart
- One bar per 100-point bucket (0-99 through 900+)
- Height proportional to count, label on top of each bar
- Exclude 999 from the histogram

**CSS Details:**
- Dark theme: `--bg: #0a0a0a`, `--surface: #141414`, `--red: #e81a1a`, `--gold: #d4a520`
- Google Fonts: `@import` Bebas Neue + Source Sans 3
- `print-color-adjust: exact; -webkit-print-color-adjust: exact;`
- `@media print` rules: hide nothing, force backgrounds
- Photos: `border-radius: 50%`, sized appropriately per section
- Max-width ~800px centered for A4-friendly printing

**Step 2: Run and verify**

```bash
cd backend && python3 generate_report.py && xdg-open report.html
```

**Step 3: Commit**

```bash
git add backend/generate_report.py
git commit -m "feat(report): full Fight Night HTML report with all stats sections"
```

**Acceptance Criteria:**
- [ ] Run script — `report.html` generated without errors
- [ ] Open in browser — all 7 sections visible and styled
- [ ] Hero shows "BAPT'HIT", date, total count
- [ ] Premier/Dernier shows correct first (id=4, 17h21, 793, Clément) and last score
- [ ] Records show correct min/max (excluding 999) with photos where available
- [ ] Julia section shows her photo and mentions her 999 scores
- [ ] Champions table is sorted by average, top 3 have medal styling, photos display
- [ ] Stats cards show mean, median, Q1, Q3 values
- [ ] Per-hour bars are proportional and labeled
- [ ] Histogram bars are proportional, labeled, 999 excluded
- [ ] Ctrl+P print preview preserves dark theme and layout

---

### Task 3: Polish and print optimization

**Files:**
- Modify: `backend/generate_report.py`

**Step 1: Print/PDF polish**

- Verify `@media print` rules work (backgrounds print, no page breaks mid-section)
- Add `page-break-inside: avoid` on cards and sections
- Add `page-break-before: auto` on major sections to allow clean breaks
- Verify photos render in print preview
- Fix any layout issues seen in Ctrl+P preview

**Step 2: Run final verification**

```bash
cd backend && python3 generate_report.py && xdg-open report.html
```

Check in browser + print preview.

**Step 3: Commit**

```bash
git add backend/generate_report.py
git commit -m "feat(report): print polish and page break optimization"
```

**Acceptance Criteria:**
- [ ] Print preview (Ctrl+P) — dark backgrounds preserved, readable
- [ ] No section is cut mid-way by a page break
- [ ] Photos render correctly in print preview
- [ ] Report fits reasonably on A4 pages (no excessive whitespace)
