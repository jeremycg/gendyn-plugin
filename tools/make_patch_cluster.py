#!/usr/bin/env python3
"""Generate GENDY3_cluster.vcv — Xenakis's actual 14-voice pitch cluster from GENDY3.

Pitches from Table 1 of Hoffmann (2022) "The Genesis of GENDY3".
Formula: freq = 44100 / (Imax * rall)

The opening section (PARAG310) used 7 of these 14 pitches.
PARAG311 (before-last section) used all 14.

Voices are pitch-FIXED (B_DUR_WIDTH ≈ 0). The stochastic walk modulates
timbre only, not pitch — this is how Xenakis got the "beautiful clear tones."
"""

import json, os, sys, io, tempfile, tarfile, subprocess, random

random.seed(77)

SR = 44100

def uid():
    return random.randint(1_000_000_000_000_000, 9_007_199_254_740_991)

def freq(imax, rall):
    return SR / (imax * rall)

# (track, imax, rall, xenakis_note)  — from Table 1, highest to lowest
ALL_VOICES = [
    (12, 16,  1,  "F7"),
    ( 8,  6,  4,  "A#6"),
    ( 1, 13,  3,  "C#6"),
    (14, 40,  1,  "C#6"),
    ( 9, 15,  6,  "B4"),
    (10, 16,  6,  "A#4"),
    ( 2, 13,  8,  "G#4"),
    ( 3, 26,  5,  "E4"),
    ( 7, 13, 17,  "G3"),
    ( 5, 13, 19,  "F3"),
    (15, 40,  9,  "B2"),
    (16, 40, 13,  "E2"),
    ( 4, 51, 19,  "F#1"),
    ( 6, 40, 29,  "D#1"),
]

# Use all 14 voices (PARAG311 "plein jeu").
# Comment out the upper 7 for the opening 7-tone cluster (PARAG310).
VOICES = ALL_VOICES

modules = []
cables  = []
gendyn_ids = []

for i, (track, imax, rall, note) in enumerate(VOICES):
    hz = freq(imax, rall)
    mid = uid()
    gendyn_ids.append(mid)
    modules.append({
        "id": mid,
        "plugin": "GENDYN",
        "model": "GENDYN",
        "version": "2.0.0",
        "params": [
            {"id": 0,  "value": float(imax)},    # N = Imax (direct integer now)
            {"id": 1,  "value": 0.02},            # SCALE_AMP — timbral evolution
            {"id": 2,  "value": 0.001},           # SCALE_DUR — near-zero, keeps pitch locked
            {"id": 3,  "value": 0.8},             # B_AMP
            {"id": 4,  "value": hz},              # B_DUR_CENTER — exact Xenakis frequency
            {"id": 5,  "value": 0.03},            # B_DUR_WIDTH — nearly fixed pitch
            {"id": 6,  "value": 1.0},             # DISTRIBUTION: Gaussian
            {"id": 7,  "value": 0.0},             # SCALE_AMP_ATT
            {"id": 8,  "value": 0.0},             # SCALE_DUR_ATT
            {"id": 9,  "value": 0.0},             # B_AMP_ATT
            {"id": 10, "value": 0.0},             # B_DUR_ATT
        ],
        "pos": [(i % 8) * 8, i // 8],
    })

# ── 4 × VCMixer (groups of 4) ─────────────────────────────────────────────────
vcmixer_ids = []
n_groups = (len(VOICES) + 3) // 4
for g in range(n_groups):
    mid = uid()
    vcmixer_ids.append(mid)
    modules.append({
        "id": mid,
        "plugin": "Fundamental",
        "model": "VCMixer",
        "version": "2.6.4",
        "params": [
            {"id": 0, "value": 0.7},
            {"id": 1, "value": 0.7},
            {"id": 2, "value": 0.7},
            {"id": 3, "value": 0.7},
            {"id": 4, "value": 0.25},
        ],
        "pos": [68 + g * 8, 0],
    })

# ── Main Mixer ────────────────────────────────────────────────────────────────
mixer_id = uid()
modules.append({
    "id": mixer_id,
    "plugin": "Fundamental",
    "model": "Mixer",
    "version": "2.6.4",
    "params": [
        {"id": 0, "value": 1.0},
        {"id": 1, "value": 1.0},
        {"id": 2, "value": 1.0},
        {"id": 3, "value": 1.0},
        {"id": 4, "value": 0.0},
        {"id": 5, "value": 0.0},
        {"id": 6, "value": 0.2},
    ],
    "pos": [100, 0],
})

# ── AudioInterface ────────────────────────────────────────────────────────────
audio_id = uid()
modules.append({
    "id": audio_id,
    "plugin": "Core",
    "model": "AudioInterface",
    "version": "2.6.6",
    "params": [],
    "data": {
        "audio": {"driver": -1, "deviceName": "", "sampleRate": 44100.0,
                  "blockSize": 256, "inputOffset": 0, "outputOffset": 0},
        "dcFilter": True
    },
    "pos": [116, 0],
})

# ── Cables ────────────────────────────────────────────────────────────────────
COLORS = ["#f3374b","#ffb437","#00b56e","#3695ef","#8b4ade","#ffffff","#c9b70e","#e55728"]
ci = 0

for i, gid in enumerate(gendyn_ids):
    group = i // 4
    ch_in = (i % 4) + 1
    cables.append({
        "id": uid(),
        "outputModuleId": gid, "outputId": 0,
        "inputModuleId": vcmixer_ids[group], "inputId": ch_in,
        "color": COLORS[ci % len(COLORS)],
        "inputPlugOrder": ci, "outputPlugOrder": ci,
    })
    ci += 1

for g, vmid in enumerate(vcmixer_ids):
    cables.append({
        "id": uid(),
        "outputModuleId": vmid, "outputId": 0,
        "inputModuleId": mixer_id, "inputId": g,
        "color": COLORS[ci % len(COLORS)],
        "inputPlugOrder": ci, "outputPlugOrder": ci,
    })
    ci += 1

for ch in range(2):
    cables.append({
        "id": uid(),
        "outputModuleId": mixer_id, "outputId": 0,
        "inputModuleId": audio_id, "inputId": ch,
        "color": "#ffffff",
        "inputPlugOrder": ci, "outputPlugOrder": ci,
    })
    ci += 1

# ── Write patch ───────────────────────────────────────────────────────────────
patch = {
    "version": "2.6.6", "zoom": 0.5, "gridOffset": [0.0, 0.0],
    "modules": modules, "cables": cables, "masterModuleId": audio_id,
}

out_dir  = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "patches")
out_file = os.path.join(out_dir, "GENDY3_cluster.vcv")

with tempfile.TemporaryDirectory() as tmp:
    json_path = os.path.join(tmp, "patch.json")
    with open(json_path, "w") as f:
        json.dump(patch, f, indent=2)
    tar_buf = io.BytesIO()
    with tarfile.open(fileobj=tar_buf, mode="w:") as tf:
        tf.add(json_path, arcname="patch.json")
    result = subprocess.run(["zstd", "-19", "-o", out_file, "-f"],
                            input=tar_buf.getvalue(), capture_output=True)
    if result.returncode != 0:
        print("zstd error:", result.stderr.decode(), file=sys.stderr); sys.exit(1)

print(f"Written: {out_file}")
print(f"  {len(VOICES)} voices, {len(modules)} modules, {len(cables)} cables")
print(f"\n  Voice pitches (Xenakis Table 1):")
for track, imax, rall, note in VOICES:
    hz = SR / (imax * rall)
    print(f"    Track {track:2d}  N={imax:2d}  rall={rall:2d}  {hz:7.1f} Hz  ({note})")

import glob
_win = glob.glob("/mnt/c/Users/*/AppData/Local/Rack2/patches")
if _win:
    import shutil
    dst = os.path.join(_win[0], "GENDY3_cluster.vcv")
    shutil.copy2(out_file, dst)
    print(f"\n  Installed: {dst}")
