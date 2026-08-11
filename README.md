# BAPT'HIT

Borne de mesure de frappe (*punch meter*) : un capteur sur ESP32 mesure les coups,
un backend central agrège les scores, et une UI web (pensée mobile) affiche le
classement, les stats, et permet de réclamer un score avec photo.

## Architecture

L'ESP32 est une **sonde** : il ne sert plus l'UI et ne stocke rien. Toute
l'intelligence vit dans le **backend central** (le laptop).

```
   ┌──────────────────────────┐         WiFi (STA)          ┌────────────────────────────┐
   │  ESP32  (sonde)           │   mDNS: bapthit.local:6969  │  Backend  (laptop central)  │
   │                           │ ──────────────────────────▶│                              │
   │  • capteur PunchMeter     │   WebSocket  /ws/device     │  • FastAPI (port 6969)       │
   │  • WiFi station           │ ◀──────────────────────────│  • SQLite  bapthit.db        │
   │  • file de scores (RAM)   │   hello / score / log       │  • photos/                   │
   │  • envoie scores + logs   │   ◀── config ──             │  • publie mDNS _bapthit._tcp │
   └──────────────────────────┘                             └──────────────┬──────────────┘
                                                                            │ HTTP same-origin
                                                                            ▼
                                                              ┌────────────────────────────┐
                                                              │  UI web  (téléphone)        │
                                                              │  http://bapthit.local:6969/ │
                                                              └────────────────────────────┘
```

Les trois composants doivent être **sur le même réseau WiFi local** (le mDNS ne
traverse pas les routeurs).

## Prérequis

> **`make` n'est pas obligatoire.** C'est juste un raccourci (Linux/macOS) autour
> de commandes Python et `idf.py`. Sous **Windows**, où `make` n'existe pas par
> défaut, lance les commandes brutes données à chaque étape. Le backend tourne
> sur **n'importe quel OS avec Python** (Windows / macOS / Linux) — Linux n'est
> pas requis.

| Composant | Besoin |
|-----------|--------|
| Backend   | Python 3.10+ (sous Linux, le paquet `python3-venv` peut être à installer) |
| Firmware  | [ESP-IDF v6.0](https://docs.espressif.com/projects/esp-idf/) installé (cible **esp32**) — sous Linux/macOS dans `~/.espressif/v6.0/esp-idf/`, sous Windows via l'installeur officiel |
| Matériel  | ESP32 + capteur PunchMeter, câble USB (puce CH340 → `/dev/ttyUSB0` sous Linux, `COMx` sous Windows) |
| Réseau    | Un WiFi 2.4 GHz commun au laptop, à l'ESP32 et au téléphone |

> **Accès au port série**
> - **Linux** : ajoute-toi au groupe `dialout` puis reconnecte ta session :
>   ```bash
>   sudo usermod -aG dialout $USER
>   ```
> - **Windows** : installe le driver CH340 si le port `COMx` n'apparaît pas dans
>   le Gestionnaire de périphériques.

### Windows depuis zéro (machine vierge)

Pour faire tourner le **backend**, la seule chose à installer au niveau système
est **Python** — `pip` et `venv` sont inclus dans l'installeur Windows
(contrairement à Linux). Aucun compilateur / Visual Studio Build Tools requis :
toutes les dépendances (`uvicorn`, `fastapi`, `Pillow` via `qrcode`, `zeroconf`…)
s'installent en wheels précompilées.

1. Installe **Python 3.11 ou 3.12** depuis [python.org](https://www.python.org/downloads/windows/)
   (évite le Microsoft Store, soucis de PATH/venv). Coche **« Add python.exe to PATH »**.
   → Préfère 3.11/3.12 à la toute dernière version : sur un Python trop récent,
   certaines wheels peuvent manquer et pip tenterait une compilation.
2. Lance les commandes PowerShell de l'**Étape 1** ci-dessous. C'est tout.

> `uvicorn[standard]` essaie d'installer `uvloop`, **indisponible sous Windows** :
> pip le saute et uvicorn retombe sur `asyncio`. C'est normal, pas une erreur.

## Installation

Le capteur PunchMeter est un **submodule git**. Clone avec `--recurse-submodules` :

```bash
git clone --recurse-submodules git@github.com:Loulen/Bapthit.git
cd Bapthit
```

Si le dépôt est déjà cloné sans le submodule :

```bash
git submodule update --init --recursive
```

---

## Étape 1 — Démarrer le backend (laptop)

Le backend crée automatiquement `bapthit.db` (avec la config par défaut du
PunchMeter) et le dossier `photos/` au premier lancement. Rien à initialiser.

**Avec `make`** (Linux/macOS) :

```bash
make backend-install   # crée backend/.venv + installe requirements.txt (une seule fois)
make backend           # lance FastAPI sur 0.0.0.0:6969
```

**Sans `make`** — commandes brutes équivalentes (à lancer depuis la racine du projet) :

```bash
# Linux / macOS
python3 -m venv backend/.venv                                # une seule fois
backend/.venv/bin/pip install -r backend/requirements.txt    # une seule fois
backend/.venv/bin/python backend/server.py                   # lance le backend
```

```powershell
# Windows (PowerShell)
python -m venv backend\.venv                                              # une seule fois
backend\.venv\Scripts\python.exe -m pip install -r backend\requirements.txt   # une seule fois
backend\.venv\Scripts\python.exe backend\server.py                        # lance le backend
```

Au démarrage, le backend publie son service mDNS et affiche son IP :

```
Publishing mDNS: bapthit.local -> 192.168.x.x:6969
```

L'UI est alors accessible à **http://bapthit.local:6969/** (ou via l'IP affichée).

Pour l'arrêter : `make backend-stop`, ou simplement **Ctrl+C** dans le terminal
du backend (la seule option sous Windows).

---

## Étape 2 — Configurer les identifiants de l'ESP32

Les identifiants WiFi ne sont **jamais** committés. Copie l'exemple et remplis-le :

```bash
cp sdkconfig.defaults.local.example sdkconfig.defaults.local
```

Édite `sdkconfig.defaults.local` (gitignoré) :

```ini
CONFIG_BAPTHIT_WIFI_SSID="ton-wifi"
CONFIG_BAPTHIT_WIFI_PASS="ton-mot-de-passe"
CONFIG_BAPTHIT_BACKEND_HOST="bapthit.local"   # mDNS du backend (Étape 1)
CONFIG_BAPTHIT_BACKEND_PORT=6969
```

Ce fichier est lu en overlay au-dessus de `sdkconfig.defaults` à la compilation.
Le backend lit aussi ce fichier pour générer le QR code de connexion WiFi
(`/api/qr/wifi`), affiché dans l'UI pour faire rejoindre le réseau aux invités.

---

## Étape 3 — Compiler et flasher le firmware

Assure-toi qu'ESP-IDF v6.0 est installé (les cibles `make` sourcent
`~/.espressif/v6.0/esp-idf/export.sh` automatiquement).

```bash
make build                      # compile
make flash                      # flashe sur PORT (défaut /dev/ttyUSB0)
make monitor                    # console série (Ctrl+] pour quitter)
make flash-monitor              # flashe puis ouvre le moniteur
```

Port différent ? Surcharge `PORT` :

```bash
make flash PORT=/dev/ttyACM0
```

> **Sans `make` (et sous Windows)** — `make` ne fait que charger l'environnement
> ESP-IDF puis appeler `idf.py`. Charge l'environnement toi-même, puis lance
> `idf.py` directement :
> - **Linux/macOS** : `. ~/.espressif/v6.0/esp-idf/export.sh` une fois dans le terminal.
> - **Windows** : ouvre le raccourci **« ESP-IDF PowerShell »** (ou **« ESP-IDF CMD »**)
>   installé par l'installeur ESP-IDF — l'environnement y est déjà chargé.
>
> ```bash
> idf.py build                          # compile
> idf.py -p /dev/ttyUSB0 flash          # flashe (Windows : -p COM3)
> idf.py -p /dev/ttyUSB0 monitor        # moniteur (Ctrl+] pour quitter)
> idf.py -p /dev/ttyUSB0 flash monitor  # flashe puis moniteur
> ```

Au boot, le moniteur doit afficher la connexion WiFi puis :

```
sent hello (boot=...)
```

→ l'ESP est connecté au backend en WebSocket. Les scores remontent désormais en
temps réel dans l'UI.

---

## Étape 4 — Ouvrir l'UI sur un téléphone

Sur un téléphone connecté au **même WiFi** :

1. Ouvre **http://bapthit.local:6969/**
   (si le mDNS ne résout pas depuis le mobile, utilise `http://<IP-du-laptop>:6969/`).
2. L'UI affiche les derniers scores, les stats, et permet de réclamer un score
   (nom + photo). Le panneau réglages contient les QR codes (rejoindre le WiFi /
   ouvrir l'UI) et la config du PunchMeter.

---

## (Optionnel) Générer le bilan HTML

`generate_report.py` produit un rapport HTML autonome (scores + stats + photos
embarquées en base64) à partir de `bapthit.db` et `photos/` :

```bash
# Linux / macOS
backend/.venv/bin/python backend/generate_report.py
# → backend/report.html
```

```powershell
# Windows (PowerShell)
backend\.venv\Scripts\python.exe backend\generate_report.py
```

> `report.html` est **gitignoré** (il contient des photos de participant·es,
> comme `photos/` et `bapthit.db`). Ne le committe pas.

---

## Référence rapide des commandes (`make` ↔ brut)

Les commandes `idf.py` supposent l'environnement ESP-IDF chargé (`export.sh` sous
Linux/macOS, raccourci « ESP-IDF PowerShell/CMD » sous Windows). Port :
`/dev/ttyUSB0` sous Linux, `COMx` sous Windows. Sous Windows, remplace
`backend/.venv/bin/` par `backend\.venv\Scripts\` et `python3` par `python`.

| Cible `make` | Effet | Équivalent sans `make` |
|--------------|-------|------------------------|
| `make backend-install` | Crée le venv et installe les dépendances Python | `python3 -m venv backend/.venv` puis `backend/.venv/bin/pip install -r backend/requirements.txt` |
| `make backend` | Lance le backend FastAPI (port 6969) | `backend/.venv/bin/python backend/server.py` |
| `make backend-stop` | Arrête le backend | `Ctrl+C` dans le terminal (ou `pkill -f backend/server.py` sous Linux/macOS) |
| `make build` | Compile le firmware ESP32 | `idf.py build` |
| `make flash` | Flashe le firmware (`PORT=/dev/ttyUSB0` par défaut) | `idf.py -p /dev/ttyUSB0 flash` |
| `make monitor` | Ouvre le moniteur série | `idf.py -p /dev/ttyUSB0 monitor` |
| `make flash-monitor` | Flashe puis ouvre le moniteur | `idf.py -p /dev/ttyUSB0 flash monitor` |
| `make clean` | `idf.py fullclean` | `idf.py fullclean` |

## Référence rapide de l'API backend

| Méthode | Route | Usage |
|---------|-------|-------|
| `GET` | `/` | UI web (sert `backend/static/index.html`) |
| `WS` | `/ws/device` | Canal ESP32 ↔ backend (hello / score / log / config) |
| `GET` | `/api/scores/history` | Historique des scores |
| `POST` | `/api/scores/claim` | Réclamer un score (nom) |
| `GET` / `POST` | `/api/config` | Lire / écrire la config PunchMeter (poussée à l'ESP) |
| `GET` | `/api/logs?since=` | Logs PunchMeter remontés par l'ESP |
| `GET` / `POST` | `/api/photos/{score_id}` | Récupérer / envoyer la photo d'un score |
| `GET` | `/api/qr/wifi` | QR code de connexion WiFi (PNG) |
| `GET` | `/api/qr/url` | QR code vers l'UI (PNG) |

## Dépannage

| Symptôme | Piste |
|----------|-------|
| `bapthit.local` ne résout pas | Backend lancé ? Même WiFi ? Sinon, utilise l'IP affichée au démarrage. |
| ESP ne se connecte pas au WiFi | Vérifie SSID/PASS dans `sdkconfig.defaults.local`, recompile (`make build flash`). WiFi 2.4 GHz uniquement. |
| ESP connecté au WiFi mais pas au backend | `CONFIG_BAPTHIT_BACKEND_HOST/PORT` corrects ? Backend démarré avant l'ESP ? |
| `Permission denied` sur `/dev/ttyUSB0` | Groupe `dialout` (voir Prérequis), puis reconnexion de session. |
| `components/punchmeter` vide | `git submodule update --init --recursive`. |
