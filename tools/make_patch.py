#!/usr/bin/env python3
"""Generate GENDY3_16voice.vcv patch file for VCV Rack 2."""

import json, random, struct, subprocess, os, sys

random.seed(42)

def uid():
    """Generate a positive 53-bit int as a Rack-style module/cable ID."""
    return random.randint(1_000_000_000_000_000, 9_007_199_254_740_991)

# ── Voice table ──────────────────────────────────────────────────────────────
# (N, center_hz, b_amp_att, b_dur_att)
VOICES = [
    ( 7,   80, 0.2, 0.4),   # 1
    (11,   95, 0.2, 0.4),   # 2
    (13,  110, 0.2, 0.4),   # 3
    (17,  130, 0.2, 0.4),   # 4
    (19,  155, 0.4, 0.4),   # 5
    (23,  185, 0.4, 0.4),   # 6
    (29,  220, 0.4, 0.4),   # 7
    (31,  260, 0.4, 0.4),   # 8
    ( 7,  310, 0.5, 0.3),   # 9
    (11,  370, 0.5, 0.3),   # 10
    (13,  440, 0.5, 0.3),   # 11
    (17,  520, 0.5, 0.3),   # 12
    (19,  620, 0.6, 0.2),   # 13
    (23,  740, 0.6, 0.2),   # 14
    (29,  880, 0.6, 0.2),   # 15
    (31, 1050, 0.6, 0.2),   # 16
]

CABLE_COLORS = [
    "#f3374b", "#ffb437", "#00b56e", "#3695ef",
    "#8b4ade", "#ffffff", "#c9b70e", "#e55728",
]

def cable_color(i):
    return CABLE_COLORS[i % len(CABLE_COLORS)]

modules = []
cables  = []

# ── GENDYN voices ─────────────────────────────────────────────────────────────
# Row 0: voices 1-8  (col 0,8,16,...,56)
# Row 1: voices 9-16 (col 0,8,16,...,56)
gendyn_ids = []
for i, (N, center_hz, b_amp_att, b_dur_att) in enumerate(VOICES):
    row = i // 8
    col = (i % 8) * 8
    mid = uid()
    gendyn_ids.append(mid)
    modules.append({
        "id": mid,
        "plugin": "GENDYN",
        "model": "GENDYN",
        "version": "2.0.0",
        "params": [
            {"id": 0,  "value": float(N)},          # N (breakpoints)
            {"id": 1,  "value": 0.0075},           # SCALE_AMP (0.35x old 0.022: second-order walk)
            {"id": 2,  "value": 0.0075},           # SCALE_DUR (0.35x old 0.022: second-order walk)
            {"id": 3,  "value": 0.8},              # B_AMP
            {"id": 4,  "value": float(center_hz)}, # B_DUR_CENTER
            {"id": 5,  "value": 0.4},              # B_DUR_WIDTH
            {"id": 6,  "value": 3.0},              # DISTRIBUTION (Logistic)
            {"id": 7,  "value": 0.0},              # SCALE_AMP_ATT
            {"id": 8,  "value": 0.0},              # SCALE_DUR_ATT
            {"id": 9,  "value": b_amp_att},        # B_AMP_ATT
            {"id": 10, "value": b_dur_att},        # B_DUR_ATT
        ],
        "pos": [col, row],
    })

# ── 4 × Fundamental::VCMixer  (groups of 4 voices, 7HP each) ─────────────────
# Inputs: 0=CH1, 1=CH2, 2=CH3, 3=CH4 audio  Outputs: 0=MIX
# Params: 0-3=ch levels (0.06 each for ~unity sum), 4=master(1.0)
vcmixer_ids = []
for g in range(4):
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

# ── Fundamental::Mixer  (sums the 4 group outputs, 13HP) ─────────────────────
# Inputs: 0-5=channels  Outputs: 0=MIX
# Params: 0-5=ch levels, 6=master
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

# ── 2 × Fundamental::LFO  (slow barrier CV, ~0.008 Hz ≈ 125 sec period) ──────
# Fundamental LFO FREQ_PARAM=-7 → very slow
# Outputs: 0=SIN, 1=TRI, 2=SAW, 3=SQR
# Unipolar (OFFSET_PARAM=1) so output is 0–10V for full barrier modulation
lfo_amp_id = uid()
lfo_dur_id = uid()
for mid, freq_offset in [(lfo_amp_id, -7.0), (lfo_dur_id, -6.8)]:
    modules.append({
        "id": mid,
        "plugin": "Fundamental",
        "model": "LFO",
        "version": "2.6.4",
        "params": [
            {"id": 0, "value": 1.0},    # OFFSET: unipolar (0–10V)
            {"id": 1, "value": 0.0},    # INVERT: normal
            {"id": 2, "value": freq_offset},  # FREQ
            {"id": 3, "value": 0.0},    # FM
            {"id": 4, "value": 0.0},    # WAVE: sine
            {"id": 5, "value": 0.0},    # RESET
            {"id": 6, "value": 0.5},    # PW
            {"id": 7, "value": 0.0},    # PWM
        ],
        "pos": [114 + (0 if mid == lfo_amp_id else 10), 0],
    })

# ── Core::AudioInterface ──────────────────────────────────────────────────────
# Inputs: 0=L, 1=R  (signals going OUT to audio hardware)
audio_id = uid()
modules.append({
    "id": audio_id,
    "plugin": "Core",
    "model": "AudioInterface",
    "version": "2.6.6",
    "params": [],
    "data": {
        "audio": {
            "driver": -1,
            "deviceName": "",
            "sampleRate": 44100.0,
            "blockSize": 256,
            "inputOffset": 0,
            "outputOffset": 0
        },
        "dcFilter": True
    },
    "pos": [130, 0],
})

# ── Cables ────────────────────────────────────────────────────────────────────
ci = 0  # color index

# GENDYN AUDIO_OUT(0) → VCMixer channel inputs
for i, gid in enumerate(gendyn_ids):
    group  = i // 4       # which VCMixer (0-3)
    ch_in  = (i % 4) + 1  # ids 1-4 = CH1-CH4 audio (id 0 = MIX_CV_INPUT, skip it)
    cables.append({
        "id": uid(),
        "outputModuleId": gid,
        "outputId": 0,   # AUDIO_OUTPUT
        "inputModuleId": vcmixer_ids[group],
        "inputId": ch_in,
        "color": cable_color(ci),
        "inputPlugOrder": ci,
        "outputPlugOrder": ci,
    })
    ci += 1

# VCMixer outputs(0) → Mixer channels 0-3
for g, vmid in enumerate(vcmixer_ids):
    cables.append({
        "id": uid(),
        "outputModuleId": vmid,
        "outputId": 0,
        "inputModuleId": mixer_id,
        "inputId": g,
        "color": cable_color(ci),
        "inputPlugOrder": ci,
        "outputPlugOrder": ci,
    })
    ci += 1

# Mixer out(0) → AudioInterface L and R
for ch in range(2):
    cables.append({
        "id": uid(),
        "outputModuleId": mixer_id,
        "outputId": 0,
        "inputModuleId": audio_id,
        "inputId": ch,
        "color": cable_color(ci),
        "inputPlugOrder": ci,
        "outputPlugOrder": ci,
    })
    ci += 1

# LFO_AMP SIN out(0) → each GENDYN B_AMP_INPUT(2)
for gid in gendyn_ids:
    cables.append({
        "id": uid(),
        "outputModuleId": lfo_amp_id,
        "outputId": 0,    # SIN output
        "inputModuleId": gid,
        "inputId": 2,     # B_AMP_INPUT
        "color": "#e55728",
        "inputPlugOrder": ci,
        "outputPlugOrder": ci,
    })
    ci += 1

# LFO_DUR SIN out(0) → each GENDYN B_DUR_INPUT(3)
for gid in gendyn_ids:
    cables.append({
        "id": uid(),
        "outputModuleId": lfo_dur_id,
        "outputId": 0,    # SIN output
        "inputModuleId": gid,
        "inputId": 3,     # B_DUR_INPUT
        "color": "#3695ef",
        "inputPlugOrder": ci,
        "outputPlugOrder": ci,
    })
    ci += 1

# ── Assemble patch JSON ───────────────────────────────────────────────────────
patch = {
    "version": "2.6.6",
    "zoom": 0.5,
    "gridOffset": [0.0, 0.0],
    "modules": modules,
    "cables": cables,
    "masterModuleId": audio_id,
}

patch_json = json.dumps(patch, indent=2)

# ── Write as tar+zstd .vcv ────────────────────────────────────────────────────
out_dir  = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "patches")
out_file = os.path.join(out_dir, "GENDY3_16voice.vcv")

# Write patch.json to temp, then tar | zstd
import tempfile, tarfile, io

with tempfile.TemporaryDirectory() as tmp:
    json_path = os.path.join(tmp, "patch.json")
    with open(json_path, "w") as f:
        f.write(patch_json)

    tar_buf = io.BytesIO()
    with tarfile.open(fileobj=tar_buf, mode="w:") as tf:
        tf.add(json_path, arcname="patch.json")
    tar_data = tar_buf.getvalue()

    # pipe through zstd -19
    result = subprocess.run(
        ["zstd", "-19", "-o", out_file, "-f"],
        input=tar_data,
        capture_output=True,
    )
    if result.returncode != 0:
        print("zstd error:", result.stderr.decode(), file=sys.stderr)
        sys.exit(1)

print(f"Written: {out_file}")
print(f"  {len(modules)} modules, {len(cables)} cables")
print(f"  Size: {os.path.getsize(out_file)} bytes")

# Also copy to Windows Rack patches folder
import glob
_win = glob.glob("/mnt/c/Users/*/AppData/Local/Rack2/patches")
if _win:
    import shutil
    dst = os.path.join(_win[0], "GENDY3_16voice.vcv")
    shutil.copy2(out_file, dst)
    print(f"  Installed to Windows Rack: {dst}")
