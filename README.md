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

| Composant | Besoin |
|-----------|--------|
| Backend   | Python 3.10+ (`python3-venv`) |
| Firmware  | [ESP-IDF v6.0](https://docs.espressif.com/projects/esp-idf/) installé dans `~/.espressif/v6.0/esp-idf/`, cible **esp32** |
| Matériel  | ESP32 + capteur PunchMeter, câble USB (puce CH340 → `/dev/ttyUSB0` sous Linux) |
| Réseau    | Un WiFi 2.4 GHz commun au laptop, à l'ESP32 et au téléphone |

> **Linux — accès au port série**
> Ajoute-toi au groupe `dialout` puis reconnecte ta session :
> ```bash
> sudo usermod -aG dialout $USER
> ```

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

```bash
make backend-install   # crée backend/.venv + installe requirements.txt (une seule fois)
make backend           # lance FastAPI sur 0.0.0.0:6969
```

Au démarrage, le backend publie son service mDNS et affiche son IP :

```
Publishing mDNS: bapthit.local -> 192.168.x.x:6969
```

L'UI est alors accessible à **http://bapthit.local:6969/** (ou via l'IP affichée).

Pour l'arrêter : `make backend-stop`.

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
backend/.venv/bin/python backend/generate_report.py
# → backend/report.html
```

> `report.html` est **gitignoré** (il contient des photos de participant·es,
> comme `photos/` et `bapthit.db`). Ne le committe pas.

---

## Référence rapide des cibles `make`

| Cible | Effet |
|-------|-------|
| `make backend-install` | Crée le venv et installe les dépendances Python |
| `make backend` | Lance le backend FastAPI (port 6969) |
| `make backend-stop` | Arrête le backend |
| `make build` | Compile le firmware ESP32 |
| `make flash` | Flashe le firmware (`PORT=/dev/ttyUSB0` par défaut) |
| `make monitor` | Ouvre le moniteur série |
| `make flash-monitor` | Flashe puis ouvre le moniteur |
| `make clean` | `idf.py fullclean` |

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
