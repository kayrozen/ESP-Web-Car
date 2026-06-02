# Plan de développement — ESP32 WROVER Radio Internet → Bluetooth A2DP

> Document de conception initial. Rédigé avant le début de l'implémentation.
> L'implémentation réelle peut diverger sur certains points (ex. fdk-aac a remplacé FAAD2 pour des raisons de licence — voir README).

---

## 1. Objectif

Un device ESP32-WROVER autonome qui :

1. **Au premier démarrage**, guide l'utilisateur à travers un setup en deux étapes depuis un navigateur (sans app) : configurer les identifiants WiFi et l'URL de la station radio, puis scanner et appairer une enceinte Bluetooth.
2. **À chaque démarrage suivant**, se connecte automatiquement au hotspot WiFi sauvegardé, reconnecte l'enceinte Bluetooth, et démarre le streaming — zéro interaction.

Cas d'usage cible : une voiture. Branché sur USB au contact, le device se réveille avec l'allumage, se connecte au hotspot du téléphone, se connecte à l'enceinte BT, et joue.

**Priorité de conception : robustesse et résilience.** C'est un device headless sans surveillance qui perd l'alimentation constamment (cycles d'allumage), opère dans un environnement RF bruité, et n'a ni écran ni clavier pour la récupération. Il ne doit jamais se coincer dans un état nécessitant un ordinateur pour s'en sortir. Chaque mode d'échec doit s'auto-récupérer. La résilience est une exigence de premier ordre, pas une réflexion après coup — voir §7.

---

## 2. Matériel

| Composant | Spec | Pourquoi |
|---|---|---|
| **ESP32-WROVER-E-N8R8** | 8MB Flash + 8MB PSRAM, ESP32-D0WD-V3 | PSRAM obligatoire pour la coexistence WiFi + BT Classic + buffering audio ; 8MB flash pour l'OTA A/B (§18) |
| Alimentation | USB voiture (5V) | S'allume/s'éteint avec le contact |

> ⚠️ **Le module WROOM ne fonctionnera PAS** — pas de PSRAM, la coexistence + buffering audio en dépendent.
> ⚠️ **L'ESP32-S3 ne fonctionnera PAS** — pas de Bluetooth Classic (BR/EDR), donc pas d'A2DP. Utiliser l'ESP32 original (WROVER).

**Pièce choisie : ESP32-WROVER-E-N8R8** (8MB flash / 8MB PSRAM). L'ancien **WROVER-B** est NRND (Not Recommended for New Designs) par Espressif ; le **WROVER-E** est l'équivalent activement supporté et pin-compatible.

**Marge mémoire (vérifiée, pas supposée) :**
- **PSRAM** — l'ESP32-D0WD ne peut adresser que **4MB** des 8MB via cache. Cette fenêtre est largement suffisante : buffers WiFi/lwIP (~150KB) + ring buffers (96KB) + travail MP3 (~30KB) + AAC avec SBR off (~300KB) = bien moins de 1MB. Le PSRAM physique supplémentaire est du slack gratuit.
- **Flash** — le binaire (BT + WiFi + MP3 + AAC + resampler, sans LMS/affichage/EQ) est ~1.5–2MB. Le flash 8MB rend le layout **A/B OTA** (deux slots app + recovery factory) trivial, où 4MB serait trop juste. C'est la raison concrète pour laquelle le N8R8 vaut le détour.

**Note alimentation (critique pour la résilience)** : les ports USB de voiture sont électriquement bruyants et peuvent sagger au démarrage du moteur. Ajouter un condensateur généreux (≥470µF) sur le rail 5V et compter sur le brownout detector (§7) pour un reset propre plutôt qu'un glitch. Préférer un port commuté sur le contact ou un port toujours alimenté pour un démarrage prévisible.

---

## 3. Pourquoi ESP-IDF, pas Arduino

WiFi + A2DP simultanément sur un seul ESP32 est fragile. Le framework Arduino ne peut pas le faire de manière fiable — la propre documentation de la librairie A2DP de pschatzmann dit de ne pas s'attendre à ce que WiFi et A2DP fonctionnent ensemble. Squeezelite-esp32 prouve que c'est faisable, et le fait en utilisant ESP-IDF directement :

- **Pinning de tasks** — contrôleur BT + stack host sur Core 0 ; WiFi + décodage audio sur Core 1, éliminant la contention du scheduler.
- **Placement mémoire** — buffers WiFi, lwIP et audio explicitement alloués en PSRAM.
- **Priorités FreeRTOS** — sortie audio prioritaire sur la réception réseau pour que le stream BT ne soit jamais privé de ressources.
- **Scheduler de coexistence** — `CONFIG_SW_COEXIST_ENABLE` avec arbitrage TDM configuré au niveau IDF.
- **Watchdogs & récupération** — accès direct au Task Watchdog, brownout detector, et API reset-reason nécessaires pour la couche de résilience (§7).

Ce projet est essentiellement "squeezelite-esp32 sans la dépendance LMS, plus un portail captif pour une seule URL de stream." La source de squeezelite-esp32 est la référence principale tout au long de ce plan.

---

## 4. Contrainte critique : SoftAP + BT Classic ne peuvent pas coexister

L'ESP32 **ne supporte pas** le mode WiFi SoftAP et Bluetooth Classic (BR/EDR) simultanément. C'est une limitation au niveau firmware. Cela signifie qu'on ne peut pas faire tourner le point d'accès du portail captif et scanner des devices Bluetooth en même temps.

**En revanche, WiFi STA + BT Classic est supporté** (avec PSRAM + coexistence logicielle) — ce dont le mode streaming a précisément besoin.

Cela structure tout le flux de setup en **deux phases** :

- **Phase 1** : SoftAP (BT off) → collecter WiFi + URL stream.
- **Phase 2** : WiFi STA (BT on) → scanner et appairer l'enceinte Bluetooth.

---

## 5. Flux complet de setup et d'exécution

```
PREMIER DÉMARRAGE (ou après reset de config)
│
├─► PHASE 1 — Portail WiFi          [SoftAP actif, Bluetooth OFF]
│     • SoftAP "CarRadio-Setup"
│     • DNS hijack → 192.168.4.1 (portail captif s'ouvre automatiquement)
│     • Page 1 : réseau WiFi + mot de passe + URL stream radio
│     • Valider, sauvegarder en NVS → phase = PHASE2 → reboot
│
├─► PHASE 2 — Appairage Bluetooth   [WiFi STA actif, Bluetooth ON]
│     • Connexion au hotspot sauvegardé (mode STA)
│     • Servir la Page 2 à carradio.local (mDNS)
│     • "Scan" → esp_bt_gap_start_discovery() → liste devices → navigateur
│     • L'utilisateur sélectionne l'enceinte → sauvegarder nom + MAC
│                                              → phase = READY → reboot
│
└─► MODE STREAMING                  [WiFi STA + Bluetooth, à chaque boot suivant]
      • Init Bluetooth (Core 0) EN PREMIER, puis WiFi (Core 1)
      • Connexion WiFi STA → connexion A2DP au MAC sauvegardé
      • Pipeline audio → stream en boucle, auto-healing sur toute défaillance
```

**Reset de config** : maintenir le bouton BOOT (GPIO0) 3 secondes au démarrage → effacer NVS → reboot en Phase 1.

---

## 6. Machine d'états

Chaque état a une sortie d'échec définie — pas d'impasses.

```
[BOOT]
   │ nvs_flash_init(); vérifier reset reason; charger config; lire phase
   │
   ├─ phase=PHASE1 (pas de creds WiFi) ──► [SOFTAP_PORTAL]
   │     BT: non démarré   |   WiFi: SoftAP uniquement
   │     Serveur HTTP sert la Page 1
   │     POST /save → valider → stocker wifi_*, stream_url
   │                  phase=PHASE2 → esp_restart()
   │
   ├─ phase=PHASE2 (creds WiFi, pas de BT) ──► [STA_BT_PAIRING]
   │     WiFi: STA connexion hotspot   |   BT: initialisé (Core 0)
   │     ├─ Connexion WiFi échoue (90s) → retour portail PHASE1
   │     Serveur HTTP sert la Page 2 à carradio.local
   │     GET  /bt_scan   → découverte → liste JSON
   │     POST /bt_select → stocker bt_name, bt_mac
   │                       phase=READY → esp_restart()
   │
   └─ phase=READY ──► [INIT_BLUETOOTH]          ← BT avant WiFi (§8)
         │
         ▼
       [WIFI_CONNECT] ──échec (retry/backoff, puis 5 min)──► reboot
         │  succès
         ▼
       [BT_CONNECT] ──échec (retry/backoff, puis 5 min)──► reboot
         │  succès
         ▼
       [STREAMING] ◄──────────────┐
         │                        │ auto-recover (pas de reboot
         ├─ stream drop ──────────┤  sauf si recovery épuisé)
         ├─ WiFi drop ────────────┤
         └─ BT drop ──────────────┘
```

---

## 7. Stratégie de résilience (exigence centrale)

### 7.1 Watchdog — récupérer de tout blocage

- Activer le **Task Watchdog Timer (TWDT)** avec un timeout généreux (30s) couvrant l'opération légitime la plus lente (connexion stream lente sur hotspot faible).
- Abonner les tasks longue durée : fetch HTTP, decode/resample, et le superviseur principal. Chacune appelle `esp_task_wdt_reset()` à chaque itération de boucle.
- Si une task se bloque (socket coincé, boucle infinie, deadlock), le TWDT déclenche un reset propre du chip.
- Garder le **RTC Watchdog** activé pendant le boot pour qu'un blocage en démarrage précoce se récupère aussi.

### 7.2 Brownout detector — resets propres sur sag d'alimentation

- Activer le brownout detector (`CONFIG_ESP_BROWNOUT_DET=y`). Le démarrage du moteur fait sagger le rail ; un reset brownout propre est bien meilleur qu'un glitch mi-alimenté qui corromprait l'état. Associer au condensateur d'entrée (§2).

### 7.3 Garde boot-loop — ne jamais se coincer dans un cycle de reboot

Le plus grand risque pour un device headless qui reboot automatiquement est une boucle de boot. Se protéger avec un compteur de reboot persistant :

- Au boot, lire `reset_reason` (`esp_reset_reason()`) et un `boot_fail_count` stocké en NVS / mémoire RTC.
- Incrémenter `boot_fail_count` tôt dans le boot. Le remettre à 0 seulement après **60 secondes de streaming réussi**.
- Si `boot_fail_count` dépasse un seuil (ex. 5), supposer que la config sauvegardée est mauvaise → **retour automatique au portail Phase 1** au lieu de rebooter à nouveau. Même une URL de stream corrompue ou un hotspot injoignable ne peut pas bricker définitivement le device.

### 7.4 Récupération de connexion — backoff exponentiel partout

Les trois connexions (WiFi, A2DP, stream HTTP) utilisent le même schéma : retry avec backoff exponentiel, plafonné, puis escalade.

```
tentative 1 → attendre 1s
tentative 2 → attendre 2s
tentative 3 → attendre 4s
tentative 4 → attendre 8s
tentative 5 → attendre 16s
...plafonné à 30s, continuer les retries
après 5 minutes d'échec total → esp_restart() (ardoise propre)
```

- **Drop WiFi** : le superviseur catch l'événement disconnect, met le pipeline audio en pause, et reconnecte avec backoff. Le hotspot pas encore disponible (l'utilisateur monte encore dans la voiture) est le cas *attendu* — le backoff le gère silencieusement.
- **Drop BT** (enceinte hors portée / éteinte) : pause pipeline, tentative de reconnexion A2DP au MAC sauvegardé avec backoff. Quand l'enceinte revient, la lecture reprend automatiquement.
- **Drop stream** (problème serveur, TCP coupé) : fermer et rouvrir le stream HTTP avec backoff. Re-résoudre la playlist m3u/pls au cas où le serveur aurait changé d'endpoint.

### 7.5 Gestion des underruns audio — dégrader, ne pas crasher

- Si `pcm_ringbuf` est en underrun (le décodage ne suit pas, ou WiFi bloqué), le callback A2DP **sort du silence** plutôt que de bloquer ou d'envoyer des données périmées. Un moment de silence est acceptable ; un callback BT bloqué ne l'est pas (il couperait le lien A2DP).
- Le pipeline log les underruns ; des underruns fréquents déclenchent une reconnexion du stream (la source a peut-être dégradé).

### 7.6 Intégrité NVS — survivre à une coupure d'alimentation en cours d'écriture

- Les écritures NVS sont intrinsèquement wear-levellées et transactionnelles dans ESP-IDF, mais les écritures de config n'ont lieu qu'à des points bien définis (sauvegarde portail, transition de phase), jamais en cours de stream.
- Au chargement, **valider chaque champ** : SSID non vide, URL stream bien formée (`http://` ou `https://`), MAC parseable. Tout champ invalide → traiter la config comme incomplète → retomber sur la phase de portail appropriée. Une config à moitié écrite ne peut jamais démarrer un stream cassé.
- Garder un byte `config_version` pour que le firmware futur puisse migrer ou rejeter des layouts obsolètes.

### 7.7 Stabilité mémoire — survivre à des jours d'uptime

- Longue durée de fonctionnement + reconnexions répétées = fragmentation heap. Mitigations :
  - Allouer les gros buffers (ring buffers, zones de travail des décodeurs) **une seule fois au démarrage en PSRAM**, jamais par connexion. La reconnexion réutilise les mêmes buffers.
  - L'état du décodeur/resampler est réinitialisé, pas réalloué, lors d'un changement de stream.
  - Le superviseur vérifie périodiquement `esp_get_free_heap_size()` et `heap_caps_get_largest_free_block()` ; si la mémoire libre passe sous un seuil critique, il fait un `esp_restart()` contrôlé (propre, rapide, inaudible lors d'un arrêt) plutôt d'attendre l'échec d'une allocation.

### 7.8 Récapitulatif des points de surveillance

| Défaillance | Détection | Récupération |
|---|---|---|
| Task bloquée / deadlock | Timeout TWDT | Reset chip propre |
| Sag d'alimentation (démarrage moteur) | Brownout detector | Reset propre + condo d'entrée |
| Boot loop / mauvaise config | NVS `boot_fail_count` | Retour auto au portail |
| Hotspot pas encore disponible | Timeout connexion WiFi | Retry backoff silencieux |
| WiFi coupé en streaming | Événement WiFi | Pause + reconnect backoff |
| Enceinte éteinte / hors portée | Callback A2DP disconnect | Pause + reconnect backoff |
| Problème serveur stream | Erreur / EOF HTTP | Réouverture + re-resolve playlist |
| Buffer underrun | Ring buffer vide | Silence de sortie, reconnect si fréquent |
| Config corrompue/partielle | Validation des champs au chargement | Retour portail |
| Fragmentation heap | Surveillance seuil heap | Restart contrôlé |

---

## 8. Ordre d'initialisation

En mode streaming, **Bluetooth s'initialise avant WiFi**. Le contrôleur BT doit réserver son allocation PSRAM avant que le stack WiFi commence à allouer dans le même pool ; initialiser WiFi en premier peut laisser un PSRAM contigu insuffisant pour le contrôleur BT.

Ordre : `esp_bt_controller_init` → `esp_bluedroid_init` → `esp_a2d_source_init` → enregistrer callbacks → `esp_wifi_init` (STA) → connect.

---

## 9. Layout des tasks dual-core

```
Core 0                          Core 1
──────────────────────          ────────────────────────────
BT Controller (IDF)             WiFi Stack (IDF)
BT Host / Bluedroid             Supervisor Task (machine d'états,
A2DP Source Task                  backoff, health checks, WDT)
  └─ pull PCM depuis            HTTP Client Task (WDT-watched)
     pcm_ringbuf                  └─ écrit raw_ringbuf
                                Decode + Resample Task (WDT-watched)
                                  └─ lit raw_ringbuf
                                  └─ décode (MP3/AAC)
                                  └─ mono→stéréo, resample
                                  └─ écrit pcm_ringbuf
```

Garder les stacks logiciels des deux radios sur des cœurs opposés est la technique clé qui rend la coexistence stable — c'est ce que fait squeezelite-esp32 et ce qu'Arduino ne peut pas faire. La **task superviseur** est propriétaire de la récupération : elle surveille les événements de connexion, pilote le backoff, exécute les health checks, et décide quand rebooter.

---

## 10. Pipeline audio (bout à bout)

```
Internet (HTTP)
    │  bytes compressés (MP3 ou AAC)
    ▼
[HTTP Client Task — Core 1]        ← WDT-watched ; rouvre sur erreur
    ▼
[raw_ringbuf — 32 KB, PSRAM]       ← absorbe les gaps TDM côté WiFi
    ▼
[Decode + Resample Task — Core 1]  ← WDT-watched
    │  1. détecter le format (MP3 / AAC-LC / HE-AAC)
    │  2. décoder la frame → PCM au taux source
    │  3. mono → stéréo (dupliquer canal) si nécessaire
    │  4. resampler à 44100 Hz si le taux source diffère
    ▼
[pcm_ringbuf — 64 KB, PSRAM]       ← toujours 44100 Hz / stéréo / 16-bit
    ▼                                 underrun → callback sort du silence
[A2DP Source Callback — Core 0]    ← absorbe les gaps TDM côté BT ; ne bloque jamais
    ▼
Enceinte Bluetooth voiture
```

Deux ring buffers PSRAM en série — un pour les bytes compressés, un pour le PCM décodé — découplent le débit WiFi du débit BT et absorbent les gaps de time-slicing du partage radio. Les buffers sont alloués une fois au démarrage et réutilisés à travers les reconnexions.

### 10a. Détection de format

```c
typedef enum { FORMAT_UNKNOWN, FORMAT_MP3, FORMAT_AAC, FORMAT_AAC_HE } stream_format_t;

// 1. Vérifier HTTP Content-Type :
//    audio/mpeg → MP3 | audio/aac → AAC-LC | audio/aacp → HE-AAC
// 2. Fallback : sniff sync word dans les premiers bytes :
//    0xFFFB / 0xFFF3 / 0xFFF2 → MP3
//    0xFFF1 / 0xFFF9          → ADTS AAC
```

### 10b. Décodeurs

| Format | Décodeur | Notes |
|---|---|---|
| MP3 | **Helix** (`helix-mp3`) | Porté depuis squeezelite-esp32 |
| AAC-LC | **FAAD2** (`libfaad`) | Standard ESP32 ; porté depuis squeezelite-esp32 |
| HE-AAC (SBR) | FAAD2 avec SBR | **Gourmand en CPU** — voir ci-dessous |

De nombreuses stations radio streament en AAC (surtout HE-AAC/AAC+ à bas bitrate), donc le support AAC est nécessaire pour une large compatibilité.

**Mise en garde SBR** : HE-AAC utilise la Spectral Band Replication pour reconstruire la bande haute fréquence et un taux d'échantillonnage plus élevé. Le SBR est optionnel selon la spec — on peut décoder seulement la bande basse. Le décodage SBR est très gourmand en CPU et consomme la marge nécessaire pour la coexistence WiFi+BT. **Livrer avec `CONFIG_AAC_DISABLE_SBR=y`** ; n'activer qu'en option avancée si le budget CPU le permet.

### 10c. Resampling (obligatoire)

A2DP exige **exactement 44100 Hz, stéréo, 16-bit**. Les streams arrivent à des taux variés :

| Taux stream | Action |
|---|---|
| 44100 Hz | Passthrough |
| 48000 Hz (courant pour AAC) | Downsampling → 44100 |
| 32000 / 22050 Hz | Upsampling → 44100 |

Envoyer directement du 48000 Hz à l'A2DP joue ~8.8% trop vite (effet chipmunk) — le resampling n'est pas optionnel.

**Resampler : SpeexDSP** (porté depuis squeezelite-esp32). Gère des ratios arbitraires (48000→44100 n'est pas entier), conçu pour le traitement temps réel par chunks, assez léger pour l'ESP32, licence BSD. Utiliser le **niveau de qualité 4** — défaut de squeezelite-esp32 ; bon équilibre audio/CPU.

```c
if (channels == 1) { /* dupliquer mono → stéréo en place */ }
if (source_rate != 44100) {
    speex_resampler_process_interleaved_int(rs, pcm_in, &in_len, pcm_out, &out_len);
    xRingbufferSend(pcm_ringbuf, pcm_out, out_len * sizeof(int16_t), portMAX_DELAY);
} else {
    xRingbufferSend(pcm_ringbuf, pcm_in, in_len * sizeof(int16_t), portMAX_DELAY);
}
```

---

## 11. Pages du portail captif

### Page 1 — WiFi + Stream (mode SoftAP, BT off)

Servie à `http://192.168.4.1` ; s'ouvre automatiquement via la détection captive portal sur iOS/Android.

```
┌─────────────────────────────────┐
│  CarRadio Setup — Étape 1/2     │
├─────────────────────────────────┤
│  Réseau WiFi   [ dropdown ▼ ]   │  ← endpoint /scan peuple la liste
│  Mot de passe  [____________]   │
│  URL du stream [____________]   │
│  [   Connecter & Continuer →]   │
└─────────────────────────────────┘
```

À la soumission : **valide** les champs (SSID non vide, URL bien formée), sauvegarde, met `phase=PHASE2`, reboot.
Le navigateur affiche : *"Connexion à votre WiFi… puis ouvrir carradio.local"* (avec fallback IP).

### Page 2 — Appairage Bluetooth (mode STA, BT on)

Servie à `http://carradio.local` (mDNS) une fois connecté au hotspot.

```
┌─────────────────────────────────┐
│  CarRadio Setup — Étape 2/2     │
├─────────────────────────────────┤
│  [   🔍 Scanner les devices ]   │
│  ○ JBL Charge 5        -62 dBm  │
│  ● Sony ULT FIELD 1    -48 dBm  │  ← utilisateur sélectionne
│  ○ Bose SoundLink      -71 dBm  │
│  [     Appairer & Terminer ✓]   │
└─────────────────────────────────┘
```

`GET /bt_scan` → démarre la découverte GAP (~8s), retourne JSON de {nom, MAC, RSSI}.
`POST /bt_select` → sauvegarde `bt_name` + `bt_mac`, met `phase=READY`, reboot.

**Appairage par MAC** : le scan capture le MAC de l'enceinte, et le mode streaming se connecte par MAC (pas par nom) — sans ambiguïté, et robuste aux changements de nom d'affichage.

---

## 12. Config persistante (namespace NVS `"carradio"`)

| Clé | Type | Description |
|---|---|---|
| `config_version` | u8 | Version du layout pour migration/validation |
| `phase` | u8 | 0 = besoin WiFi, 1 = besoin BT, 2 = prêt |
| `wifi_ssid` | str | SSID du hotspot |
| `wifi_pass` | str | Mot de passe du hotspot |
| `stream_url` | str | URL du stream radio |
| `bt_name` | str | Nom d'affichage de l'enceinte Bluetooth |
| `bt_mac` | str | MAC Bluetooth (`AA:BB:CC:DD:EE:FF`) |
| `boot_fail_count` | u8 | Garde boot-loop (§7.3) |

Chaque champ est validé au chargement ; toute valeur invalide force un retour à la phase de portail appropriée.

---

## 13. Composants

| Composant | Source | Rôle |
|---|---|---|
| `esp_wifi` | IDF intégré | SoftAP + STA |
| `esp_http_server` | IDF intégré | Pages portail + API scan BT |
| `esp_http_client` | IDF intégré | Fetch stream radio |
| `mdns` | IDF intégré | Hostname `carradio.local` |
| `nvs_flash` | IDF intégré | Stockage config (wear-levelled) |
| `esp_task_wdt` | IDF intégré | Task watchdog (§7.1) |
| `app_update` / `esp_https_ota` | IDF intégré | OTA A/B, rollback, vérif signature (§17) |
| `bt` / `esp_a2dp_api` | IDF intégré | A2DP source |
| `esp_gap_bt_api` | IDF intégré | Découverte + appairage BT |
| `helix-mp3` | port squeezelite-esp32 | Décodage MP3 |
| `faad2 / libfaad` | port squeezelite-esp32 | Décodage AAC-LC + HE-AAC |
| `speexdsp resampler` | port squeezelite-esp32 | Conversion taux → 44100 Hz |
| `achimpieters/esp32-captive_portal` | ESP Component Registry | DNS hijack Phase 1 |

---

## 14. Paramètres sdkconfig clés

```ini
CONFIG_ESP32_DEFAULT_CPU_FREQ_240=y

# PSRAM
CONFIG_SPIRAM_SUPPORT=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_SPEED_80M=y

# Buffers WiFi en PSRAM, task WiFi sur Core 1
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=4
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=16
CONFIG_ESP_WIFI_TASK_CORE_ID=1

# Bluetooth sur Core 0
CONFIG_BT_ENABLED=y
CONFIG_BT_CLASSIC_ENABLED=y
CONFIG_BT_A2DP_ENABLE=y
CONFIG_BT_SPP_ENABLED=n
CONFIG_BTDM_CTRL_PINNED_TO_CORE=0
CONFIG_BT_BLUEDROID_PINNED_TO_CORE=0

# Coexistence logicielle WiFi/BT (équilibre)
CONFIG_SW_COEXIST_ENABLE=y
CONFIG_SW_COEXIST_PREFERENCE_VALUE=2

# Résilience
CONFIG_ESP_TASK_WDT_EN=y
CONFIG_ESP_TASK_WDT_TIMEOUT_S=30
CONFIG_ESP_TASK_WDT_PANIC=y          # timeout WDT → reset
CONFIG_ESP_BROWNOUT_DET=y
CONFIG_BOOTLOADER_WDT_ENABLE=y       # RTC WDT pendant le boot

# OTA + rollback (voir §18)
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK=n
CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT=y
CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=n

# AAC : désactiver SBR par défaut pour la marge CPU
CONFIG_AAC_DISABLE_SBR=y

# Table de partitions personnalisée (flash 8MB, OTA A/B)
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y
```

### Table de partitions (`partitions.csv`) — flash 8MB, OTA A/B

```
# Nom,       Type, SousType, Offset,    Taille,   Flags
nvs,         data, nvs,      0x9000,    0x6000,
otadata,     data, ota,      0xf000,    0x2000,
phy_init,    data, phy,      0x11000,   0x1000,
factory,     app,  factory,  0x20000,   0x280000,  # 2.5MB image recovery
ota_0,       app,  ota_0,    0x2A0000,  0x280000,  # 2.5MB slot app A
ota_1,       app,  ota_1,    0x520000,  0x280000,  # 2.5MB slot app B
storage,     data, nvs,      0x7A0000,  0x40000,   # config + logs
```

Ce layout A/B (plus une image factory de recovery) est le cœur de la stratégie OTA grade résilience :
- Deux slots app de 2.5MB — le nouveau firmware est écrit sur le slot inactif pendant que le slot actif continue de tourner.
- Une image `factory` de recovery — un build minimal connu comme bon que le bootloader peut toujours utiliser si les deux slots OTA sont mauvais.
- La partition `otadata` (deux secteurs, sûre contre les coupures d'alimentation) enregistre quel slot boote ensuite.
- Le flash 8MB rend cela confortable ; avec 4MB et notre taille binaire ce ne serait pas possible.

---

## 15. Structure des fichiers

```
esp32-car-radio/
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig.defaults
├── main/
│   ├── CMakeLists.txt
│   ├── main.c                 ← app_main, boot-fail guard, reset-reason
│   ├── config.h               ← constantes, GPIO defs, tailles buffers, timeouts
│   ├── supervisor.c / .h      ← machine d'états, backoff, health checks, WDT
│   ├── storage.c / .h         ← NVS lecture/écriture, validation, phase, fail-count
│   ├── portal_phase1.c / .h   ← SoftAP + DNS + Page 1 (WiFi + URL)
│   ├── portal_phase2.c / .h   ← Serveur HTTP STA + Page 2 (scan/pair BT)
│   ├── wifi.c / .h            ← SoftAP + STA, connect, reconnect, mDNS
│   ├── bluetooth.c / .h       ← init A2DP, scan GAP, connect par MAC, callbacks
│   ├── http_stream.c / .h     ← fetch HTTP, détection format, résolveur m3u/pls
│   ├── mp3_decode.c / .h      ← wrapper Helix
│   ├── aac_decode.c / .h      ← wrapper FAAD2 (AAC-LC + SBR optionnel)
│   ├── resample.c / .h        ← wrapper SpeexDSP + mono→stéréo
│   ├── ota.c / .h             ← OTA A/B, vérif signature, confirm rollback
│   └── audio_pipeline.c / .h  ← ring buffers, tasks, dispatch format, underrun
└── components/
    ├── helix-mp3/             ← depuis squeezelite-esp32
    ├── faad2/                 ← depuis squeezelite-esp32
    └── speexdsp/              ← depuis squeezelite-esp32
```

---

## 16. Build & Flash

```bash
idf.py --version          # 4.4.x recommandé
idf.py set-target esp32
idf.py menuconfig         # vérifier PSRAM, BT, coexist, WDT, brownout, OTA, AAC, flash 8MB
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Ce projet a **deux cibles de build** : l'app principale (flashée dans `factory` initialement, puis auto-mise à jour dans `ota_0`/`ota_1` via §17) et l'**image factory de recovery** minimale (Phase 15). L'image de recovery est buildée et flashée dans le slot `factory` une fois en fabrication ; c'est l'app principale qui sera mise à jour par OTA ensuite.

Recommandé : l'**extension IDF VS Code** officielle d'Espressif — gère toolchain, menuconfig UI, et flash/monitor dans un seul endroit.

---

## 17. Mises à jour OTA firmware (grade résilience)

Pour un device vivant sans surveillance dans une voiture, le mécanisme de mise à jour est lui-même une feature de résilience : il doit pouvoir corriger des bugs en production, et il doit être **impossible qu'une mauvaise mise à jour brique le device**.

### 17.1 Livraison des mises à jour

Le device a déjà un portail captif et un stack HTTP. L'OTA les réutilise :

- Une petite **page "Firmware"** dans l'UI web (`carradio.local`) affiche la version courante et un bouton "Mettre à jour".
- Options de source de mise à jour :
  - **Upload manuel** : l'utilisateur sélectionne un `.bin` dans le navigateur → POST vers le device → écrit dans le slot inactif. Le plus simple ; pas de serveur nécessaire.
  - **Pull depuis URL** : le device fetch un `.bin` signé depuis une URL HTTPS via `esp_https_ota`. Permet les "check for updates" sans laptop.
- Les mises à jour tournent **uniquement à l'arrêt/inactif**, jamais en cours de route — le superviseur refuse de démarrer un OTA pendant le streaming actif car les écritures flash entrent en compétition avec le pipeline audio.

### 17.2 Écriture → vérification → swap → confirmation (séquence sûre)

```
1. esp_ota_begin() sur le slot INACTIF (ota_0 ou ota_1 — jamais celui qui tourne)
2. Stream l'image en chunks → esp_ota_write()   [TWDT désinscrit pendant les écritures]
3. esp_ota_end()  → valide le magic image + signature (optionnel)
4. esp_ota_set_boot_partition(slot_inactif)
5. esp_restart()
6. Le nouveau firmware boot en état PENDING_VERIFY
7. S'il atteint "streaming OK pendant 60s" → esp_ota_mark_app_valid_cancel_rollback()
8. S'il crashe / se bloque / ne confirme jamais → le bootloader revient automatiquement
   au slot précédent au prochain boot (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
```

Propriété de sécurité clé : une nouvelle image n'est rendue permanente **qu'après avoir prouvé qu'elle peut réellement streamer** (l'étape 7 réutilise le même signal "bon boot de 60 secondes" que la garde boot-loop en §7.3).

### 17.3 Fallback en couches

```
ota_0 / ota_1  →  opération A/B normale, auto-rollback entre eux
       │ les deux slots sont mauvais ?
       ▼
image factory  →  build de recovery minimal connu-bon (WiFi + portail + OTA seulement,
                  sans audio) qui peut toujours re-flasher une bonne app par air
```

### 17.4 Intégrité & sécurité

- **Vérification de signature** : signer les images de release ; le device vérifie la signature avant de swapper. Empêche une image corrompue ou altérée d'être bootée.
- **HTTPS uniquement** pour les mises à jour en mode pull (`CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=n`).
- **Gestion watchdog** : désinscrire la task OTA du TWDT pendant les écritures flash (les écritures peuvent dépasser le timeout de 30s sur de grandes images), réinscrire après.
- **Sécurité contre les coupures d'alimentation** : la partition `otadata` est sur deux secteurs ; une coupure de courant au milieu d'un swap ne peut pas corrompre le sélecteur de boot.
- **Suivi de version** : stocker la version firmware en NVS et dans la struct `esp_app_desc` ; la page Firmware l'affiche ; refuser d'installer une version identique ou plus ancienne sauf si forcé.

### 17.5 La config survit aux mises à jour

La config vit dans sa propre partition NVS `storage`, séparée des slots app. L'OTA ne la touche jamais — les creds WiFi, l'URL stream, et l'appairage BT persistent à travers les mises à jour firmware.

---

## 18. Phases d'implémentation

| # | Tâche | Fichiers | Effort |
|---|---|---|---|
| 1 | Scaffold + menuconfig + table partitions A/B | `CMakeLists.txt`, `sdkconfig.defaults`, `partitions.csv` | 1–2h |
| 2 | Stockage NVS + validation + phase + fail-count + version | `storage.c` | 2h |
| 3 | Portail Phase 1 (SoftAP + DNS + Page 1 + validation champs) | `portal_phase1.c` | 3–4h |
| 4 | WiFi STA connect + reconnect + backoff + mDNS | `wifi.c` | 2–3h |
| 5 | Portail Phase 2 (scan BT + Page 2) | `portal_phase2.c` | 4–5h |
| 6 | BT A2DP source init + connect par MAC + reconnect | `bluetooth.c` | 3–4h |
| 7 | Fetch HTTP + détection format + résolveur m3u/pls + réouverture | `http_stream.c` | 3–4h |
| 8 | Décodage MP3 (Helix) | `mp3_decode.c` | 2h |
| 9 | Décodage AAC (FAAD2) + flag SBR | `aac_decode.c` | 3–4h |
| 10 | Resampler SpeexDSP + mono→stéréo | `resample.c` | 2–3h |
| 11 | Pipeline ring buffer + tasks + dispatch + gestion underrun | `audio_pipeline.c` | 4–5h |
| 12 | Superviseur : machine d'états, backoff, health checks, câblage WDT | `supervisor.c` | 4–5h |
| 13 | Résilience : boot-fail guard, brownout, reset-reason, surveillance heap | `main.c`, `supervisor.c` | 3–4h |
| 14 | OTA : mise à jour A/B, vérif signature, rollback, page web Firmware | `ota.c`, `portal_*.c` | 4–6h |
| 15 | Image factory de recovery (build minimal WiFi+portail+OTA) | cible de build séparée | 3–4h |
| 16 | Tests de soak incl. OTA : cycles allumage, perte RF, kills serveur, rollback bad-update, uptime multi-jours | — | 7–14h |
| | **Total** | | **~50–71h** |

Les phases 1–5 sont les fondations. Les phases 6–11 sont le cœur audio. Les phases 12–13 sont la couche de résilience. Les phases 14–15 sont la couche mise à jour/recovery. **La phase 16 est non-négociable pour ce device** — la résilience n'est prouvée qu'en cassant délibérément les choses, y compris en poussant un firmware délibérément cassé et en confirmant l'auto-rollback.

---

## 19. Plan de test de résilience (détail Phase 16)

| Test | Méthode | Critère de succès |
|---|---|---|
| Cycling allumage | Couper/rétablir l'alimentation 50× rapidement | Boot toujours jusqu'au streaming ; pas de boot loop |
| Brownout | Sagger l'alimentation à ~3.0V brièvement | Reset propre, récupère ; pas de corruption |
| Mauvaise config | Flasher une URL stream invalide | Retour auto au portail en 5 boots |
| Hotspot tardif | Alimenter le device avant d'activer le hotspot | Se connecte silencieusement une fois le hotspot disponible |
| Perte WiFi en streaming | Désactiver le hotspot 2 min, réactiver | L'audio se met en pause, reprend automatiquement |
| Enceinte hors portée | Éloigner l'enceinte, la ramener | Reconnecte, reprend automatiquement |
| Kill serveur stream | Bloquer/killer l'endpoint stream | Rouvre / re-résout ; récupère quand c'est revenu |
| Task bloquée | Injecter un blocage artificiel dans une task | TWDT reset en 30s |
| Uptime multi-jours | Tourner en continu 72h+ | Pas d'épuisement heap, pas de dégradation |
| Tempête d'underruns | Throttler le réseau au quasi-arrêt | Sort du silence, récupère ; ne perd jamais le lien BT |
| Bon OTA | Pousser une image plus récente valide | Met à jour, confirme après 60s, config préservée |
| Mauvais OTA (crash) | Pousser une image qui crashe au boot | Auto-rollback vers slot précédent, toujours fonctionnel |
| Mauvais OTA (ne stream pas) | Pousser une image qui boot mais ne stream pas | Jamais confirmée → rollback au prochain boot |
| Coupure alimentation mid-OTA | Débrancher pendant l'écriture flash | L'ancien slot boot toujours ; pas de corruption |
| Deux slots mauvais | Corrompre les deux slots app | Boot sur image factory de recovery ; récupérable par OTA |
| Image altérée | Pousser une image non signée/altérée | Vérification signature rejette ; pas de swap |

---

## 20. Risques & mitigations

| Risque | Mitigation |
|---|---|
| SoftAP + BT Classic ne peuvent pas coexister | Setup en deux phases : Phase 1 WiFi-only, Phase 2 STA+BT |
| Dropouts audio WiFi + BT pendant le streaming | Ring buffers PSRAM, split Core 0/1, tuning TDM coexist |
| Device bloqué sans personne pour le réparer | TWDT + boot-fail guard + recovery superviseur (§7) |
| Boot loop depuis mauvaise config | NVS `boot_fail_count` → retour auto portail (§7.3) |
| Sag alimentation au démarrage moteur | Brownout detector + condensateur d'entrée (§2, §7.2) |
| Fragmentation heap sur des jours | Buffers alloués une fois ; restart contrôlé seuil bas-eau (§7.7) |
| HE-AAC SBR surcharge le CPU | Livrer `AAC_DISABLE_SBR=y` ; fallback auto sous underrun |
| Stream 48 kHz → effet chipmunk | Resampler SpeexDSP obligatoire dans le pipeline |
| Stream mono sonne mal en A2DP | Duplication mono→stéréo avant resampler |
| URL stream est une playlist m3u/pls | Résolveur dans `http_stream.c`, re-résolu à la reconnexion |
| Utilisateur ne peut pas atteindre `carradio.local` | Afficher IP de fallback sur la page "veuillez patienter" |
| Scan BT ne retourne rien | Mettre l'enceinte en mode découvrable d'abord ; bouton retry ; fenêtre 8s |
| Mise à jour firmware brique le device | Slots A/B + auto-rollback + recovery factory (§17) |
| Coupure mid-update corrompt le boot | `otadata` deux secteurs ; écriture slot inactif atomique (§17.4) |
| Image de mise à jour altérée/corrompue | Images signées, vérifiées avant swap ; pull HTTPS (§17.4) |
| Binaire trop large | Slots app 2.5MB sur flash 8MB ; désactiver profils BT inutilisés (SPP/HFP) |
| Hotspot iOS nécessite activation manuelle | Documenter ; Android Auto Hotspot est totalement automatique |
| Licensing décodeur (FAAD2 LGPL) | Vérifier contre les besoins de licence du projet ; alt : fdk-aac |

---

## 21. Fichiers de référence dans squeezelite-esp32

| Notre module | Référence squeezelite-esp32 |
|---|---|
| `bluetooth.c` | `components/bluetooth/bt_app_av.c`, `bt_app_core.c` |
| `mp3_decode.c` | `components/squeezelite/helixmp3.c` |
| `aac_decode.c` | `components/squeezelite/faad.c` |
| `resample.c` | `components/squeezelite/resample.c` |
| `http_stream.c` | `components/squeezelite/stream.c` |
| `audio_pipeline.c` | `components/squeezelite/output_bt.c` |
| `ota.c` | `components/squeezelite-ota/squeezelite-ota.c` |
| `portal_phase1/2.c` | `components/wifi-manager/` (référence de pattern) |
| `sdkconfig.defaults` | `squeezelite-esp32-I2S-4MFlash-sdkconfig.defaults` |
