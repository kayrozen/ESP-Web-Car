# ESP-ADF Prototype — Validation HLS + A2DP Source

Prototype d'architecture pour répondre à la question : **ESP-ADF peut-il remplacer
squeezelite-esp32 + proxy HLS sur un WROVER-E-N8R8 ?**

Voir `../plan.md` (section ESP-ADF) pour le contexte complet.

---

## Prérequis

### Versions figées

```
ESP-ADF : release/v2.7  (commit à documenter ici après installation)
ESP-IDF : v5.3.x        (embarqué dans esp-adf, $ADF_PATH/esp-idf)
```

### Installation

```bash
git clone --recursive https://github.com/espressif/esp-adf.git $HOME/esp/esp-adf
cd $HOME/esp/esp-adf
git checkout release/v2.7
git submodule update --init --recursive
# Notez le commit exact :
git log -1 --format="%H  %ai" > /tmp/adf-version.txt
cat /tmp/adf-version.txt
```

### Variables d'environnement

```bash
export ADF_PATH=$HOME/esp/esp-adf
export IDF_PATH=$ADF_PATH/esp-idf
source $IDF_PATH/export.sh
```

---

## Configuration

Avant de compiler, renseignez vos identifiants dans `main/app_config.h` :

```c
#define WIFI_SSID   "votre_ssid"
#define WIFI_PASS   "votre_mot_de_passe"
#define BT_SINK_NAME "NOM_DU_HAUT_PARLEUR"   // ex: "JBL Flip 6"
```

Ou passez-les via `menuconfig` → Component config → ADF Prototype Config.

---

## Compilation et flash

```bash
cd adf-prototype
idf.py set-target esp32
idf.py menuconfig        # vérifier : Custom board, pas LyraT
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

---

## Phases de test

### Phase A — Vérification des exemples ESP-ADF bruts

Avant de toucher ce prototype, vérifier que les exemples ADF fonctionnent
isolément sur le WROVER :

```bash
cd $ADF_PATH/examples/player/pipeline_living_stream
idf.py build flash monitor   # HLS → I2S/DAC

cd $ADF_PATH/examples/player/pipeline_a2dp_source_stream
idf.py build flash monitor   # SD card → A2DP
```

Critères : les deux exemples jouent de l'audio. Sinon : problème toolchain,
régler avant d'avancer.

### Phase B — Fusion réseau → A2DP (ce prototype)

Le firmware démarre sur `STATION_IDX_MP3_ICECAST` (MP3 Icecast, le plus simple).

**Séquence de test :**

1. Flasher, ouvrir le moniteur série
2. Observer : WiFi connect → BT connect → "FIRST SOUND latency: XXX ms"
3. Écouter 10 min → vérifier absence de coupures
4. Changer de station dans le code (`STATION_IDX_AAC_ICECAST`) → recompiler → retester
5. Changer pour `STATION_IDX_HLS_AAC` → **le test décisif**

**Critères Phase B :**
- [ ] MP3 Icecast : 10 min sans coupure
- [ ] AAC Icecast : 10 min sans coupure
- [ ] HLS AAC     : joue et reste stable → verdict GO potentiel

### Phase C — Mesures (logs `stats_monitor`)

Le task `stats` logue toutes les 30 s :

```
I (30012) stats: --- STATS @ 30s ---
I (30013) stats:   RAM internal :  142312 B free  (min 138240 B)
I (30014) stats:   RAM SPIRAM   : 6291456 B free  (min 6123456 B)
I (30015) stats:   Glitches: 0   Reconnects: 0
```

**Critères Phase C :**
- [ ] ≥ 30 % RAM interne libre en régime (≥ ~100 KB sur 327 KB)
- [ ] ≥ 1.5 MB PSRAM libre en régime
- [ ] Pas de tendance à la baisse sur 1h (pas de fuite)
- [ ] Latence premier son HTTP < 5 s, HLS < 15 s
- [ ] < 2 glitches/heure sur AAC Icecast, < 5/heure sur HLS

### Phase D — Extensibilité

Voir les TODO dans `main.c` marqués `/* PHASE D */` pour les tests à activer :
- Élément custom passthrough (compteur d'octets)
- Switch de station à chaud (`switch_station()`)
- Serveur HTTP parallèle
- Commandes AVRCP

---

## Tableau de résultats (à remplir)

| Métrique | Valeur mesurée | Critère | Résultat |
|---|---|---|---|
| RAM interne libre (régime) | — | ≥ 100 KB | — |
| PSRAM libre (régime) | — | ≥ 1.5 MB | — |
| Fuite mémoire sur 1h | — | Aucune | — |
| Latence premier son MP3 | — | < 5 s | — |
| Latence premier son HLS | — | < 15 s | — |
| Coupures/h AAC Icecast | — | < 2 | — |
| Coupures/h HLS AAC | — | < 5 | — |
| BT reste connecté au switch | — | Oui ou < 3 s | — |
| Serveur HTTP coexiste | — | Oui | — |
| AVRCP play/pause reçu | — | Oui | — |

---

## Verdict (à compléter après Phase C)

**GO / NO-GO / HYBRIDE** :

_À remplir._

---

## Points de friction notés

_(Documenter ici chaque fois qu'on doit lire le code source ADF faute de doc.)_

- [ ] …
