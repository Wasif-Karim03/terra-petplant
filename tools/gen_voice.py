#!/usr/bin/env python3
# gen_voice.py — (re)generate Aiko's voice clips into data/audio/ with edge-tts.
# The voice character (en-US-AnaNeural) is fixed in voicelib.py so every unit
# sounds identical. Only the name changes per unit.
#
#   OWNER="Tasfia" python3 tools/gen_voice.py     # named unit
#   OWNER=""       python3 tools/gen_voice.py     # name-agnostic (for selling)
#
# Needs edge-tts (see tools/VOICE_PIPELINE.md). Existing clips are skipped;
# delete data/audio/L*.mp3 first to fully regenerate for a new name.
import os, sys, subprocess
sys.path.insert(0, os.path.dirname(__file__))
import voicelib as V

PROJ  = V.project_root()
SRC   = os.path.join(PROJ, "src", "main.cpp")
OUT   = os.path.join(PROJ, "data", "audio")
TTS   = os.environ.get("EDGE_TTS", os.path.join(PROJ, "tools", "venv", "bin", "edge-tts"))
OWNER = os.environ.get("OWNER", V.OWNER_DEFAULT)

os.makedirs(OUT, exist_ok=True)
if not os.path.exists(TTS):
    sys.exit(f"edge-tts not found at {TTS}. See tools/VOICE_PIPELINE.md to set it up "
             f"(or set EDGE_TTS=/path/to/edge-tts).")

manifest = {}
for raw in V.extract_lines(SRC):
    if not V.normalize(V.apply_owner(raw, OWNER)):
        continue
    lid = V.clip_id(raw, OWNER)
    if lid in manifest:
        continue
    spoken = V.speakify(raw, OWNER)
    if spoken:
        manifest[lid] = spoken

print(f"voice={V.VOICE}  rate={V.RATE}  owner={OWNER!r}  -> {len(manifest)} clips in {OUT}")
made = skipped = 0
for lid, text in manifest.items():
    path = os.path.join(OUT, lid + ".mp3")
    if os.path.exists(path) and os.path.getsize(path) > 0:
        skipped += 1; continue
    r = subprocess.run([TTS, f"--voice={V.VOICE}", f"--rate={V.RATE}",
                        f"--text={text}", f"--write-media={path}"], capture_output=True)
    if r.returncode == 0 and os.path.exists(path):
        made += 1; print(f"  + {lid}  {text[:46]}")
    else:
        print(f"  ! FAIL {lid}: {r.stderr.decode()[:120]}")
print(f"done: {made} made, {skipped} already existed")
print("next: run tools/gen_phrasebank.py (same OWNER), then `pio run -t uploadfs`")
