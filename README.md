# ESP32-WROVER — Radio Internet → Bluetooth A2DP

Firmware ESP-IDF pour streamer une radio internet sur une enceinte Bluetooth, conçu pour une utilisation autonome en voiture : se connecte au hotspot du téléphone, se connecte à l'enceinte BT de la voiture, joue sans aucune interaction.

---

## Sommaire

- [Matériel](#matériel)
- [Prérequis](#prérequis)
- [Build et flash](#build-et-flash)
- [Premier démarrage](#premier-démarrage)
- [Reset de configuration](#reset-de-configuration)
- [Mise à jour OTA](#mise-à-jour-ota)
- [Architecture technique](#architecture-technique)
- [Résilience](#résilience)
- [Composants audio](#composants-audio)
- [Structure du projet](#structure-du-projet)
- [Licences](#licences)

---

## Matériel

| Composant | Détail |
|---|---|
| **ESP32-WROVER-E-N8R8** | 8 MB Flash + 8 MB PSRAM, ESP32-D0WD-V3 |
| Alimentation | Port USB commuté sur l'allumage (5 V) |
| Condensateur | ≥ 470 µF sur le rail 5 V (protection contre les sags au démarrage moteur) |

> **WROOM : non compatible** — pas de PSRAM, coexistence WiFi+BT+audio impossible.
> **ESP32-S3 : non compatible** — pas de Bluetooth Classic (BR/EDR), donc pas d'A2DP.

Le module WROVER-E-N8R8 est le seul choix valide : PSRAM obligatoire pour les buffers audio et la coexistence radio, 8 MB flash pour le schéma A/B OTA avec image factory de secours.

---

## Prérequis

- **ESP-IDF v4.4.x** — [guide d'installation Espressif](https://docs.espressif.com/projects/esp-idf/en/v4.4/esp32/get-started/index.html)
- Python 3.8+, CMake 3.16+
- Extension VS Code **ESP-IDF** (optionnel, recommandé pour menuconfig graphique et flash/monitor intégrés)

```bash
idf.py --version   # doit afficher ESP-IDF v4.4.x
```

---

## Build et flash

Toutes les options sdkconfig sont pré-configurées dans `sdkconfig.defaults` (PSRAM, BT, coexistence, TWDT, brownout, OTA, flash 8 MB). Aucun menuconfig manuel nécessaire.

```bash
git clone <repo-url> esp32-car-radio
cd esp32-car-radio

idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Le binaire compile à ~1.5–2 MB, confortable dans les slots OTA de 2.5 MB.

Pour les flashes suivants :

```bash
idf.py -p /dev/ttyUSB0 flash          # flash seul
idf.py -p /dev/ttyUSB0 monitor        # monitor série seul
```

---

## Premier démarrage

Le setup se fait entièrement depuis un navigateur, en deux phases. SoftAP et Bluetooth Classic ne peuvent pas coexister sur l'ESP32, d'où cette séquence.

### Phase 1 — WiFi et URL du flux

1. Le device démarre en point d'accès WiFi `CarRadio-Setup`
2. Se connecter à ce réseau depuis le téléphone — le portail captif s'ouvre automatiquement sur iOS et Android
3. Si le portail ne s'ouvre pas automatiquement : aller sur `http://192.168.4.1`
4. Choisir le réseau WiFi (hotspot), entrer le mot de passe, entrer l'URL du flux radio
5. Appuyer sur **Connecter & Continuer** — le device redémarre

### Phase 2 — Appairage Bluetooth

1. Le device se connecte au hotspot en mode STA
2. Aller sur `http://carradio.local` (ou l'IP affichée dans le monitor série)
3. Appuyer sur **Scanner** et attendre ~8 secondes
4. Sélectionner l'enceinte dans la liste, appuyer sur **Appairer & Terminer**
5. Le device redémarre et entre en mode streaming automatique

À partir de là, tous les démarrages suivants sont entièrement automatiques.

---

## Reset de configuration

Maintenir le bouton **BOOT (GPIO0)** pendant **3 secondes** au démarrage. La configuration NVS est effacée et le device repart en Phase 1.

---

## Mise à jour OTA

Aller sur `http://carradio.local` → onglet **Firmware**.

**Upload manuel** : sélectionner un `.bin` → flash du slot inactif → redémarrage → confirmation automatique après 60 secondes de streaming.

**Pull depuis URL** : entrer une URL HTTPS vers le binaire signé → même séquence.

Si la nouvelle firmware plante ou ne parvient pas à streamer dans les 60 secondes, le bootloader revient automatiquement au slot précédent. Si les deux slots sont corrompus, l'image factory (portail WiFi minimal + OTA) prend le relais.

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

Les deux ring buffers PSRAM découplent le débit WiFi du débit Bluetooth et absorbent les interruptions dues au time-slicing de la coexistence radio. Ils sont alloués une seule fois au démarrage et réutilisés à travers tous les reconnects.

Le format de sortie A2DP est fixe : **44100 Hz / stéréo / 16-bit**. Le resampler est donc obligatoire — sans lui, un flux AAC à 48000 Hz jouerait 8.8 % trop vite.

### Répartition des cœurs

```
Core 0                          Core 1
──────────────────────          ──────────────────────────────
BT Controller (IDF)             WiFi Stack (IDF)
BT Host / Bluedroid             Supervisor Task
A2DP Source Task                HTTP Client Task
                                Decode + Resample Task
```

Placer les deux stacks radio sur des cœurs opposés est la technique clé qui rend la coexistence stable — c'est ce que fait squeezelite-esp32 et ce qu'Arduino ne peut pas faire.

### Ordre d'initialisation en mode streaming

Bluetooth s'initialise **avant** WiFi. Le contrôleur BT doit réserver sa PSRAM avant que le stack WiFi commence ses allocations. L'ordre inverse peut laisser des blocs PSRAM insuffisants pour le BT.

---

## Résilience

Le device est conçu pour fonctionner sans surveillance, perdre l'alimentation à chaque cycle d'allumage, opérer dans un environnement RF bruité, et ne jamais nécessiter un câble USB pour récupérer d'une situation de blocage.

| Situation | Détection | Récupération |
|---|---|---|
| Task bloquée / deadlock | Task Watchdog 30 s | Reset chip propre |
| Sag de tension (démarrage moteur) | Brownout detector | Reset propre |
| Boot loop / config corrompue | `boot_fail_count` NVS > 5 | Retour automatique Phase 1 |
| Hotspot pas encore disponible | Timeout connect WiFi | Backoff silencieux, retry |
| WiFi coupé en streaming | Événement WiFi | Pause + reconnect exponentiel |
| Enceinte hors portée / éteinte | Callback A2DP disconnect | Pause + reconnect exponentiel |
| Serveur stream inaccessible | Erreur / EOF HTTP | Réouverture + re-resolve playlist |
| Buffer underrun | Ring buffer vide | Silence, reconnect si fréquent |
| Config invalide au démarrage | Validation NVS au boot | Retour au portail de la phase concernée |
| Fragmentation heap | Seuil `esp_get_free_heap_size` | Restart contrôlé |
| OTA cassante | Pas de confirmation 60 s | Rollback automatique bootloader |
| Deux slots OTA corrompus | Bootloader | Démarrage sur image factory |

Le backoff exponentiel suit le même schéma partout : 1 s, 2 s, 4 s … plafonné à 30 s, puis redémarrage propre après 5 minutes d'échec total.

---

## Composants audio

Les trois décodeurs et le resampler sont intégrés directement dans le dépôt avec leurs sources réelles. Aucune manipulation n'est nécessaire.

### Helix MP3

Sources : [chmorgan/libhelix-mp3](https://github.com/chmorgan/libhelix-mp3) — RealNetworks open-source 2005 (RPSL/RCSL).

`assembly.h` inclut nativement la voie Xtensa (instructions `mulsh` et `abs`) — le décodage fixed-point est optimal sur ESP32 sans patch supplémentaire.

```
components/helix-mp3/
├── helix_mp3.c          ← wrapper (MP3InitDecoder / MP3Decode / MP3FreeDecoder)
├── mp3dec.c / mp3tabs.c
├── pub/                 ← mp3dec.h, mp3common.h, statname.h
└── real/                ← 13 .c + assembly.h + coder.h
```

### fdk-aac (AAC-LC / HE-AAC v1/v2 / xHE-AAC)

Sources : [mstorsjo/fdk-aac](https://github.com/mstorsjo/fdk-aac) — licence Fraunhofer (libre pour usage non-commercial).

Build décodeur uniquement — les libs encodeur, SAC et MPEG-TP encodeur sont exclus.

```
components/faad2/
├── faad2.c              ← wrapper (aacDecoder_Open / Fill / DecodeFrame)
├── include/faad2.h      ← API publique du projet
└── fdk-aac/
    ├── libAACdec/       29 .cpp  décodeur AAC principal
    ├── libSBRdec/       19 .cpp  HE-AAC v1/v2 (SBR + PS)
    ├── libDRCdec/        9 .cpp  Dynamic Range Control
    ├── libFDK/          25 .cpp  DSP bas-niveau Fraunhofer
    ├── libMpegTPDec/     6 .cpp  parsing ADTS/ADIF
    ├── libPCMutils/      3 .cpp  downmix + limiter
    ├── libArithCoding/   1 .cpp  xHE-AAC
    └── libSYS/           2 .cpp  couche système
```

Le SBR est contrôlé par `CONFIG_AAC_DISABLE_SBR` dans `sdkconfig.defaults`. Avec SBR désactivé, HE-AAC se décode à la fréquence de base (économie d'environ 30 % de CPU, utile sous coexistence WiFi+BT).

### SpeexDSP

Sources : [xiph/speexdsp](https://github.com/xiph/speexdsp) — BSD 3-Clause.

Mode fixed-point (`FLOATING_POINT=0`), qualité 4 — équilibre audio/CPU recommandé par squeezelite-esp32.

```
components/speexdsp/
├── speexdsp_resampler.c     ← wrapper (speex_resampler_init / process)
├── include/
└── libspeexdsp/             ← resample.c + speex_resampler.h + headers arch
```

---

## Structure du projet

```
esp32-car-radio/
├── CMakeLists.txt
├── partitions.csv               ← table A/B OTA pour flash 8 MB
├── sdkconfig.defaults           ← toute la config sdkconfig prête à l'emploi
├── main/
│   ├── main.c                   ← app_main, boot-fail guard, bouton BOOT, dispatch
│   ├── config.h                 ← constantes, GPIO, tailles buffers, timeouts
│   ├── storage.c / .h           ← NVS : lecture/écriture, validation, phase
│   ├── supervisor.c / .h        ← machine d'état, backoff, surveillance heap, WDT
│   ├── wifi.c / .h              ← SoftAP, STA, reconnect, mDNS
│   ├── portal_phase1.c / .h     ← Phase 1 : SoftAP + DNS hijack + formulaire WiFi/URL
│   ├── portal_phase2.c / .h     ← Phase 2 : serveur HTTP STA + scan BT + page OTA
│   ├── bluetooth.c / .h         ← A2DP source, scan GAP, connect par MAC
│   ├── http_stream.c / .h       ← fetch HTTP, résolution m3u/pls, détection format
│   ├── audio_pipeline.c / .h    ← ring buffers PSRAM, tasks, dispatch, underrun
│   ├── mp3_decode.c / .h        ← wrapper Helix
│   ├── aac_decode.c / .h        ← wrapper fdk-aac
│   ├── resample.c / .h          ← wrapper SpeexDSP
│   └── ota.c / .h               ← OTA A/B, vérification signature, confirmation rollback
└── components/
    ├── helix-mp3/               ← sources Helix complètes (RPSL/RCSL)
    ├── faad2/                   ← sources fdk-aac complètes (Fraunhofer)
    └── speexdsp/                ← sources SpeexDSP complètes (BSD 3-Clause)
```

---

## Licences

| Composant | Licence | Usage commercial |
|---|---|---|
| Helix MP3 | RealNetworks RPSL/RCSL | Vérifier les termes RPSL pour distribution |
| fdk-aac | Fraunhofer FDK | Libre pour usage non-commercial ; licence commerciale disponible |
| SpeexDSP | BSD 3-Clause | Libre |
| ESP-IDF | Apache 2.0 | Libre |

Pour une redistribution commerciale sans contrainte sur le décodeur AAC, contacter Fraunhofer pour une licence commerciale fdk-aac, ou substituer [minimp3](https://github.com/lieff/minimp3) (CC0) pour le MP3.
