# ESP32-WROVER Internet Radio → Bluetooth A2DP

Standalone ESP-IDF firmware that streams an internet radio station over WiFi and plays it on a paired Bluetooth speaker — designed for unattended use in a car (USB power on ignition, phone hotspot, car BT speaker).

---

## Table des matières

1. [Matériel requis](#1-matériel-requis)
2. [Prérequis logiciels](#2-prérequis-logiciels)
3. [Cloner et configurer](#3-cloner-et-configurer)
4. [Drop-in des composants audio tiers](#4-drop-in-des-composants-audio-tiers)
   - [4.1 Helix MP3](#41-helix-mp3)
   - [4.2 FAAD2 (AAC-LC / HE-AAC)](#42-faad2-aac-lc--he-aac)
   - [4.3 SpeexDSP (resampler)](#43-speexdsp-resampler)
5. [Build et flash](#5-build-et-flash)
6. [Premier démarrage — setup en deux phases](#6-premier-démarrage--setup-en-deux-phases)
7. [Reset de configuration](#7-reset-de-configuration)
8. [Mise à jour OTA](#8-mise-à-jour-ota)
9. [Architecture](#9-architecture)
10. [Résilience](#10-résilience)
11. [Structure des fichiers](#11-structure-des-fichiers)
12. [Licences des composants tiers](#12-licences-des-composants-tiers)

---

## 1. Matériel requis

| Composant | Spec |
|---|---|
| **ESP32-WROVER-E-N8R8** | 8 MB Flash + 8 MB PSRAM, ESP32-D0WD-V3 |
| Alimentation voiture | Port USB commuté sur l'allumage (5V) |
| Condensateur | ≥ 470 µF sur le rail 5V (résistance aux sags au démarrage) |

> **Le module WROOM ne fonctionnera PAS** — pas de PSRAM, WiFi+BT+audio impossible.
> **L'ESP32-S3 ne fonctionnera PAS** — pas de Bluetooth Classic (BR/EDR), donc pas d'A2DP.

---

## 2. Prérequis logiciels

- **ESP-IDF v4.4.x** (recommandé) — [guide d'installation Espressif](https://docs.espressif.com/projects/esp-idf/en/v4.4/esp32/get-started/index.html)
- Python 3.8+, CMake 3.16+
- Extension VS Code **ESP-IDF** (optionnel mais recommandé pour menuconfig graphique)

Vérifier l'installation :

```bash
idf.py --version
# doit afficher : ESP-IDF v4.4.x
```

---

## 3. Cloner et configurer

```bash
git clone <repo-url> esp32-car-radio
cd esp32-car-radio

idf.py set-target esp32
idf.py menuconfig          # vérifier les sections ci-dessous
```

Sections importantes dans `menuconfig` :
- **Component config → ESP32-specific → PSRAM** — activer SPIRAM
- **Component config → Bluetooth** — BT Classic + A2DP activés
- **Component config → Software coexistence** — `SW_COEXIST_ENABLE=y`
- **Serial flasher config** — Flash size = 8 MB
- **Partition Table** — Custom, fichier `partitions.csv`
- **Bootloader config → App rollback** — activer

Toutes ces options sont pré-configurées dans `sdkconfig.defaults` ; un simple `idf.py build` les applique sans menuconfig manuel.

---

## 4. Drop-in des composants audio tiers

**Helix MP3 est déjà intégré** — les sources réelles sont présentes dans `components/helix-mp3/` (téléchargées depuis [chmorgan/libhelix-mp3](https://github.com/chmorgan/libhelix-mp3), open-source RealNetworks 2005, RPSL/RCSL). Le décodeur MP3 est fonctionnel sans aucune manipulation.

Les deux composants restants (`faad2`, `speexdsp`) sont encore des stubs compilables. Pour activer l'audio AAC et le resampling réel, suivre les instructions ci-dessous.

---

### 4.1 Helix MP3 — INTÉGRÉ ✓

Les sources Helix complètes sont déjà dans `components/helix-mp3/` :

```
components/helix-mp3/
├── helix_mp3.c          ← wrapper → API interne du projet
├── mp3dec.c             ← top-level decoder (RealNetworks)
├── mp3tabs.c            ← lookup tables
├── pub/
│   ├── mp3dec.h         ← API publique Helix
│   ├── mp3common.h      ← structures internes
│   └── statname.h       ← name mangling (linking statique)
└── real/
    ├── assembly.h       ← inline asm (Xtensa/ESP32 supporté nativement)
    ├── coder.h          ← structures internes décodeur
    ├── bitstream.c / buffers.c / dct32.c / dequant.c
    ├── dqchan.c / huffman.c / hufftabs.c / imdct.c
    ├── polyphase.c / scalfact.c / stproc.c
    ├── subband.c / trigtabs.c
```

**Aucune action requise.** Le wrapper `helix_mp3.c` appelle directement `MP3InitDecoder` / `MP3Decode` / `MP3FreeDecoder`.

---

### 4.2 FAAD2 (AAC-LC / HE-AAC) — INTÉGRÉ ✓

Les sources FAAD2 complètes sont dans `components/faad2/` (téléchargées depuis [knik0/faad2](https://github.com/knik0/faad2), GPL v2) :

```
components/faad2/
├── faad2.c              ← wrapper → API interne du projet
├── include/
│   └── neaacdec.h       ← API publique FAAD2
└── libfaad/
    ├── decoder.c / bits.c / cfft.c / common.c / syntax.c ...  (38 .c)
    ├── sbr_dec.c / sbr_qmf.c / sbr_syntax.c ...               (SBR — compilé mais désactivé par défaut)
    └── structs.h / common.h / ... (48 .h)
```

**Aucune action requise.** `faad2.c` appelle `NeAACDecOpen` → `NeAACDecInit` → `NeAACDecDecode2`.

**SBR** : contrôlé par `CONFIG_AAC_DISABLE_SBR` dans `sdkconfig.defaults` (activé = SBR off = ~30% CPU en moins). Pour activer le SBR complet, changer la valeur dans `sdkconfig.defaults` puis rebuildez.

> **Licence GPL v2** : FAAD2 est GPL, ce qui impose des obligations de distribution des sources pour tout produit commercial. Alternative libre : [fdk-aac](https://github.com/mstorsjo/fdk-aac) (licence Fraunhofer, plus permissive pour usage non-commercial).

---

### 4.3 SpeexDSP (resampler) — INTÉGRÉ ✓

Le resampler SpeexDSP est dans `components/speexdsp/` (téléchargé depuis [xiph/speexdsp](https://github.com/xiph/speexdsp), BSD 3-Clause) :

```
components/speexdsp/
├── speexdsp_resampler.c          ← wrapper → API interne du projet
├── include/
│   └── speexdsp_resampler.h
└── libspeexdsp/
    ├── resample.c                ← cœur du resampler (1242 lignes)
    ├── speex_resampler.h         ← API publique xiph
    ├── arch.h / fixed_generic.h / fixed_arm4.h / fixed_arm5e.h
    ├── resample_neon.h           ← optimisations NEON ARM
    └── vorbis_psy.h
```

**Aucune action requise.** `speexdsp_resampler.c` appelle `speex_resampler_init` / `speex_resampler_process_interleaved_int` en mode **fixed-point** (entiers, pas de FPU). Qualité **4** par défaut (équilibre audio/CPU sous coexistence WiFi+BT).

---

## 5. Build et flash

```bash
# Premier flash (inclut partition table + factory slot)
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor

# Flash seul (après modifications)
idf.py -p /dev/ttyUSB0 flash

# Monitor série seul
idf.py -p /dev/ttyUSB0 monitor
```

Le binaire (~1.5–2 MB) tient confortablement dans les slots ota_0/ota_1 de 2.5 MB.

---

## 6. Premier démarrage — setup en deux phases

### Phase 1 — WiFi + URL stream

1. Le device démarre en SoftAP `CarRadio-Setup`
2. Se connecter à ce réseau depuis le téléphone — le portail captif s'ouvre automatiquement (iOS/Android)
3. Si le portail ne s'ouvre pas : naviguer manuellement vers `http://192.168.4.1`
4. Sélectionner le réseau WiFi (hotspot), entrer le mot de passe, entrer l'URL du flux radio
5. Appuyer sur **Connecter & Continuer** — le device reboot

### Phase 2 — Appairage Bluetooth

1. Le device se connecte au hotspot en STA
2. Naviguer vers `http://carradio.local` (ou l'IP affichée sur le port série)
3. Appuyer sur **Scanner** — attendre ~8 secondes
4. Sélectionner l'enceinte dans la liste, appuyer sur **Appairer & Terminer**
5. Le device reboot et entre en mode streaming automatique

**Tous les démarrages suivants** : le device se connecte automatiquement sans interaction.

---

## 7. Reset de configuration

Maintenir le bouton **BOOT (GPIO0)** enfoncé pendant **3 secondes** au démarrage → efface toute la configuration NVS → repart en Phase 1.

---

## 8. Mise à jour OTA

Naviguer vers `http://carradio.local` → onglet **Firmware**.

- **Upload manuel** : sélectionner un fichier `.bin` → POST vers le device → flash du slot inactif → reboot → confirmation automatique après 60s de streaming OK
- **Pull depuis URL** : entrer une URL HTTPS pointant vers le binaire signé

Si la nouvelle firmware plante au démarrage ou ne parvient pas à streamer dans les 60s, le bootloader **revient automatiquement** au slot précédent.

---

## 9. Architecture

```
Core 0                          Core 1
──────────────────────          ────────────────────────────────
BT Controller (IDF)             WiFi Stack (IDF)
BT Host / Bluedroid             Supervisor (state machine,
A2DP Source Task                  backoff, heap watch, WDT)
  └─ pull PCM depuis            HTTP Client Task (WDT)
     pcm_ringbuf                  └─ écrit raw_ringbuf [32KB PSRAM]
                                Decode + Resample Task (WDT)
                                  └─ lit raw_ringbuf
                                  └─ décode MP3 / AAC
                                  └─ mono→stéréo, resample 44100Hz
                                  └─ écrit pcm_ringbuf [64KB PSRAM]
```

Les deux ring buffers PSRAM découplent le débit WiFi du débit BT et absorbent le time-slicing de la coexistence radio.

**Format audio A2DP** : exactement 44100 Hz / stéréo / 16-bit — le resampler SpeexDSP convertit tous les flux (48000 Hz courant pour AAC, 32000/22050 Hz possibles).

---

## 10. Résilience

| Failure | Détection | Récupération |
|---|---|---|
| Task hang / deadlock | TWDT 30s | Reset chip propre |
| Sag tension (démarrage moteur) | Brownout detector | Reset propre + condo 470µF |
| Boot loop / config corrompue | NVS `boot_fail_count` > 5 | Retour auto Phase 1 |
| Hotspot pas encore levé | WiFi connect timeout | Backoff silencieux |
| WiFi coupé en streaming | WiFi event | Pause + reconnect backoff |
| Enceinte hors portée | A2DP disconnect cb | Pause + reconnect backoff |
| Serveur stream mort | HTTP read error/EOF | Réouverture + re-resolve playlist |
| Buffer underrun | Ring buffer vide | Silence, reconnect si fréquent |
| Config invalide/partielle | Validation au chargement | Retour portail phase appropriée |
| Fragmentation heap | Low-water check | Restart contrôlé |
| OTA crashante | Pas de confirmation 60s | Rollback auto bootloader |

---

## 11. Structure des fichiers

```
esp32-car-radio/
├── CMakeLists.txt
├── partitions.csv            ← A/B OTA, 8MB flash
├── sdkconfig.defaults        ← PSRAM, BT, coexistence, WDT, brownout...
├── main/
│   ├── main.c                ← app_main, boot-fail guard, BOOT button, dispatch
│   ├── config.h              ← toutes les constantes
│   ├── storage.c / .h        ← NVS, validation, phase, boot_fail_count
│   ├── supervisor.c / .h     ← machine d'état, backoff, santé, WDT
│   ├── wifi.c / .h           ← SoftAP + STA, mDNS
│   ├── portal_phase1.c / .h  ← Phase 1 : SoftAP + DNS hijack + Page 1
│   ├── portal_phase2.c / .h  ← Phase 2 : STA HTTP + Page 2 + OTA page
│   ├── bluetooth.c / .h      ← A2DP, GAP scan, connect by MAC
│   ├── http_stream.c / .h    ← fetch HTTP, m3u/pls, format detection
│   ├── audio_pipeline.c / .h ← ring buffers, tasks, dispatch, underrun
│   ├── mp3_decode.c / .h     ← wrapper Helix
│   ├── aac_decode.c / .h     ← wrapper FAAD2
│   ├── resample.c / .h       ← wrapper SpeexDSP
│   └── ota.c / .h            ← A/B OTA, rollback confirm
└── components/
    ├── helix-mp3/            ← stub → drop-in sources Helix (§4.1)
    ├── faad2/                ← stub → drop-in sources FAAD2 (§4.2)
    └── speexdsp/             ← stub → drop-in sources SpeexDSP (§4.3)
```

---

## 12. Licences des composants tiers

| Composant | Licence | Remarque |
|---|---|---|
| Helix MP3 | RealNetworks RPSL / commercial | Vérifier les termes pour un produit commercial |
| FAAD2 / libfaad | LGPL v2 | Lier dynamiquement ou fournir les sources modifiées |
| SpeexDSP | BSD 3-Clause | Compatible avec tout projet |
| ESP-IDF | Apache 2.0 | Compatible avec tout projet |

> Pour un produit commercial, considérer **fdk-aac** (licence Fraunhofer) comme alternative à FAAD2, ou **minimp3** (CC0/MIT) comme alternative à Helix.
