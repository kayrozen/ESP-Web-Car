# ESP32-WROVER — Radio Internet → Bluetooth A2DP

Firmware ESP-IDF pour streamer une radio internet sur une enceinte Bluetooth, conçu pour une utilisation autonome en voiture : se connecte au hotspot du téléphone, se connecte à l'enceinte BT de la voiture, joue sans aucune interaction.

---

## Sommaire

- [Matériel](#matériel)
- [Prérequis](#prérequis)
- [Page d'installation](#page-dinstallation)
- [Premier démarrage](#premier-démarrage)
- [Changer de station](#changer-de-station)
- [Reset de configuration](#reset-de-configuration)
- [Mise à jour OTA](#mise-à-jour-ota)
- [Architecture technique](#architecture-technique)
- [Résilience](#résilience)
- [Télémétrie](#télémétrie)
- [Composants audio](#composants-audio)
- [Structure du projet](#structure-du-projet)

---

## Matériel

| Composant | Détail |
|---|---|
| **ESP32-WROVER-E-N8R8** | 8 MB Flash + 8 MB PSRAM, ESP32-D0WD-V3 |
| Alimentation | Port USB commuté sur l'allumage (5 V) |
| Condensateur | ≥ 470 µF sur le rail 5 V (protection contre les sags au démarrage moteur) |

> **WROOM : non compatible** — pas de PSRAM, coexistence WiFi+BT+audio impossible.
> **ESP32-S3 : non compatible** — pas de Bluetooth Classic (BR/EDR), donc pas d'A2DP.

---

## Prérequis

- **ESP-IDF v4.4.x** — [guide d'installation Espressif](https://docs.espressif.com/projects/esp-idf/en/v4.4/esp32/get-started/index.html)
- Python 3.8+, CMake 3.16+

```bash
idf.py --version   # doit afficher ESP-IDF v4.4.x
```

---

## Page d'installation

La configuration initiale (choix des stations, nom du device) se fait via la **page d'installation** hébergée sur GitHub Pages, **avant** de brancher le device.

### Flux d'installation

```
1. Ouvrir la page d'installation dans Chrome ou Edge
   (Web Serial API requise — pas Firefox)

2. Étape 1 — Nommer le device
   Ex: "kitchen-radio" → deviendra kitchen-radio-Setup (SoftAP)
   et kitchen-radio.local (mDNS)

3. Étape 2 — Construire la playlist (1–5 stations)
   • Suggestions locales via géolocalisation + radio-browser.info
   • Recherche en direct
   • URL personnalisée possible
   • Glisser-déposer pour réordonner

4. Étape 3 — Brancher le device, cliquer "Connect & Install"
   • Flash du firmware via ESP Web Tools
   • Envoi automatique de la config sur le port série :
     PROVISION:{...}\n → le device répond OK\n
```

La page est déployée automatiquement via GitHub Actions à chaque push sur `main`. Pour l'activer : **Settings → Pages → Source → GitHub Actions**.

### Re-provisioning

Ré-exécuter la page d'installation sur un device déjà flashé remplace la playlist sans re-flasher le firmware. Le device écoute sur le port série pendant **30 secondes** à chaque boot.

---

## Premier démarrage

### Phase 1 — WiFi (SoftAP, Bluetooth OFF)

1. Le device démarre en point d'accès `<device-name>-Setup` (ex: `kitchen-radio-Setup`)
2. Se connecter à ce réseau — le portail captif s'ouvre automatiquement
3. Si le portail ne s'ouvre pas : aller sur `http://192.168.4.1`
4. Choisir le réseau WiFi, entrer le mot de passe
5. La playlist provisionnée à l'install s'affiche en lecture seule pour confirmation
6. Appuyer sur **Enregistrer & Continuer** — le device redémarre

### Phase 2 — Appairage Bluetooth (WiFi STA, Bluetooth ON)

1. Le device se connecte au hotspot en mode STA
2. Aller sur `http://<device-name>.local` (ex: `http://kitchen-radio.local`)
3. Appuyer sur **Scanner** et attendre ~8 secondes
4. Sélectionner l'enceinte, appuyer sur **Appairer & Terminer**
5. Le device redémarre et entre en mode streaming automatique

À partir de là, tous les démarrages suivants sont entièrement automatiques.

---

## Changer de station

La playlist (jusqu'à 5 stations) est navigable via les boutons **Suivant / Précédent** AVRCP de l'autoradio. Le device déclare le support de ces commandes dans le filtre passthrough AVRCP au démarrage.

- **Suivant** (`ESP_AVRC_PT_CMD_FORWARD`) → station suivante (boucle)
- **Précédent** (`ESP_AVRC_PT_CMD_BACKWARD`) → station précédente (boucle)

L'index courant est persisté en NVS à chaque changement : le device reprend la dernière station après un redémarrage.

---

## Reset de configuration

Maintenir le bouton **BOOT (GPIO0)** pendant **3 secondes** au démarrage. La configuration NVS est effacée et le device repart en Phase 1.

---

## Mise à jour OTA

Aller sur `http://<device-name>.local` → onglet **Firmware**.

**Upload manuel** : sélectionner un `.bin` → flash du slot inactif → redémarrage → confirmation automatique après 60 secondes de streaming.

**Pull depuis URL** : entrer une URL HTTPS vers le binaire signé → même séquence.

Si la nouvelle firmware plante ou ne stream pas dans les 60 secondes, le bootloader revient automatiquement au slot précédent.

---

## Architecture technique

### Pipeline audio

```
Internet (HTTP)
    │  MP3 ou AAC compressé
    ▼
[HTTP Client Task — Core 1]
    │
    ▼  raw_ringbuf  32 KB PSRAM
    │
[Decode + Resample Task — Core 1]
    │  détection format (Content-Type + sync word)
    │  décodage MP3 (Helix) ou AAC (fdk-aac)
    │  mono → stéréo si nécessaire
    │  resample → 44100 Hz (SpeexDSP, qualité 4)
    │
    ▼  pcm_ringbuf  64 KB PSRAM
    │
[A2DP Source Callback — Core 0]
    │  underrun → silence (jamais de blocage)
    ▼
Enceinte Bluetooth
```

### Répartition des cœurs

```
Core 0                          Core 1
──────────────────────          ──────────────────────────────
BT Controller (IDF)             WiFi Stack (IDF)
BT Host / Bluedroid             Supervisor Task
A2DP Source Task                HTTP Client Task
                                Decode + Resample Task
                                Telemetry Flush Task (prio 2)
                                Command Poll Task (prio 2)
```

### Ordre d'initialisation en mode streaming

Bluetooth s'initialise **avant** WiFi. Le contrôleur BT doit réserver sa PSRAM avant que le stack WiFi commence ses allocations.

---

## Résilience

| Situation | Détection | Récupération |
|---|---|---|
| Task bloquée / deadlock | Task Watchdog 30 s | Reset chip propre |
| Sag de tension (démarrage moteur) | Brownout detector | Reset propre |
| Boot loop / config corrompue | `boot_fail_count` NVS > 5 | Retour automatique Phase 1 |
| Hotspot pas encore disponible | Timeout connect WiFi | Backoff silencieux, retry |
| WiFi coupé en streaming | Événement WiFi | Pause + reconnect exponentiel |
| Enceinte hors portée / éteinte | Callback A2DP disconnect | Pause + reconnect exponentiel |
| Toutes les stations mortes | Erreur stream 3× | Auto-skip à la suivante |
| Serveur stream inaccessible | Erreur / EOF HTTP | Réouverture + re-resolve playlist |
| Buffer underrun | Ring buffer vide | Silence, reconnect si fréquent |
| Config invalide au démarrage | Validation NVS au boot | Retour au portail de la phase concernée |
| Fragmentation heap | Seuil `esp_get_free_heap_size` | Restart contrôlé |
| OTA cassante | Pas de confirmation 60 s | Rollback automatique bootloader |
| Deux slots OTA corrompus | Bootloader | Démarrage sur image factory |

---

## Télémétrie

La télémétrie est **opt-out** et activée par défaut. Elle ne touche jamais au pipeline audio.

### Événements collectés

| Type | Déclencheur |
|---|---|
| `boot` | Chaque démarrage |
| `state_transition` | Machine d'état playback |
| `bt_event` | Connexion / déconnexion A2DP |
| `avrcp_passthrough` | Commande play/pause/stop/next/prev |
| `avrcp_metadata_sent` | Push métadonnées vers la voiture |
| `icy_title` | Nouveau titre ICY parsé |
| `stream_open` | Ouverture flux HTTP |
| `stream_close` | Fermeture flux |
| `audio_underrun` | Ring buffer vide |
| `ota_event` | Phases OTA |
| `provisioning_received` | Provisioning série réussi |
| `station_switch` | Changement de station AVRCP |
| `station_play_started` | Démarrage streaming d'une station |
| `station_play_ended` | Fin streaming d'une station |

Les MACs Bluetooth et SSIDs WiFi sont hachés avec un sel par device avant transmission. Aucune donnée personnelle, aucun mot de passe.

### Architecture serveur

```
telemetry-server/
├── docker-compose.yml   Caddy + Go API + Postgres + Grafana
├── Caddyfile
├── schema.sql
├── .env.example
├── api/main.go          POST /register, /sessions, /events; GET /commands
└── grafana-provisioning/
```

---

## Composants audio

| Composant | Source | Rôle |
|---|---|---|
| Helix MP3 | [chmorgan/libhelix-mp3](https://github.com/chmorgan/libhelix-mp3) | Décodage MP3 fixed-point Xtensa |
| fdk-aac | [mstorsjo/fdk-aac](https://github.com/mstorsjo/fdk-aac) | AAC-LC / HE-AAC (SBR désactivé par défaut) |
| SpeexDSP | [xiph/speexdsp](https://github.com/xiph/speexdsp) | Resample → 44100 Hz, qualité 4, fixed-point |

Le format de sortie A2DP est fixe : **44100 Hz / stéréo / 16-bit**. Le resampler est obligatoire — un flux AAC à 48000 Hz jouerait 8.8 % trop vite sans lui.

---

## Structure du projet

```
esp32-car-radio/
├── CMakeLists.txt
├── partitions.csv              table 8 MB : factory + A/B OTA + storage + log
├── sdkconfig.defaults          config sdkconfig prête à l'emploi
├── main/
│   ├── main.c                  app_main, boot-fail guard, bouton BOOT
│   ├── config.h                constantes, GPIO, tailles buffers, clés NVS
│   ├── storage.c / .h          NVS : lecture/écriture, validation, phase
│   ├── provisioning_serial.c/h UART0 listener 30s → PROVISION: JSON → NVS
│   ├── playlist.c / .h         playlist 5 stations, NVS persist, next/prev
│   ├── supervisor.c / .h       machine d'états, backoff, WDT, OTA check
│   ├── wifi.c / .h             SoftAP, STA, reconnect, mDNS (device_name.local)
│   ├── portal_phase1.c / .h    Phase 1 : SoftAP + DNS hijack + formulaire WiFi
│   ├── portal_phase2.c / .h    Phase 2 : serveur HTTP STA + scan BT + OTA
│   ├── bluetooth.c / .h        A2DP source, scan GAP, connect par MAC
│   ├── avrcp.c / .h            AVRCP TG, passthrough play/pause/stop/next/prev
│   ├── http_stream.c / .h      fetch HTTP, résolution m3u/pls, ICY metadata
│   ├── audio_pipeline.c / .h   ring buffers PSRAM, tasks, dispatch, underrun
│   ├── mp3_decode.c / .h       wrapper Helix
│   ├── aac_decode.c / .h       wrapper fdk-aac
│   ├── resample.c / .h         wrapper SpeexDSP
│   ├── ota.c / .h              OTA A/B, rollback, log ota_event
│   ├── device_identity.c / .h  UUID + api_key NVS, POST /register
│   ├── telemetry.c / .h        ring buffer, log NVS, flush task
│   └── command_poll.c / .h     poll commands, upload_full_log, force_ota
├── components/
│   ├── helix-mp3/              sources Helix (RPSL/RCSL)
│   ├── faad2/                  sources fdk-aac (Fraunhofer)
│   └── speexdsp/               sources SpeexDSP (BSD 3-Clause)
├── install/
│   ├── index.html              page d'installation GitHub Pages (vanilla JS)
│   ├── manifest.json           ESP Web Tools firmware manifest
│   └── firmware/               binaires générés par CI (bootloader, partition-table, app)
├── telemetry-server/
│   ├── docker-compose.yml
│   ├── api/main.go
│   └── grafana-provisioning/
└── .github/workflows/
    └── pages.yml               build firmware + deploy GitHub Pages
```

### NVS namespace `"carradio"`

| Clé | Type | Description |
|---|---|---|
| `device_name` | str | Nom du device (mDNS, SoftAP SSID, BT source) |
| `playlist_json` | str | Playlist JSON (array, max 5 entrées) |
| `playlist_idx` | u8 | Index station courante (persisté entre reboots) |
| `wifi_ssid` | str | SSID du hotspot |
| `wifi_pass` | str | Mot de passe WiFi |
| `bt_name` | str | Nom de l'enceinte Bluetooth |
| `bt_mac` | str | MAC Bluetooth (`AA:BB:CC:DD:EE:FF`) |
| `phase` | u8 | 0=WiFi, 1=BT, 2=ready |
| `boot_fail_count` | u8 | Garde boot-loop |
| `stream_url` | str | Legacy — remplacé par `playlist_json` |
