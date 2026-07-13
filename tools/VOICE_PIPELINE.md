# Aiko Voice Pipeline — how to reproduce the voice for every unit

This is the permanent, version-controlled recipe for Aiko's voice. Follow it and
**every unit sounds identical** — same cute voice, same lines, same character —
only the owner's name changes (or is dropped for a name-agnostic product).

## The voice (never change these — that's what keeps units consistent)
- **Voice model:** `en-US-AnaNeural` (Microsoft edge-tts — free, no API key)
- **Rate:** `-8%` (slightly slower = cozy)
- Both are pinned in `tools/voicelib.py` (`VOICE`, `RATE`).

## Where things live
- **Phrasebank (the lines):** inside `src/main.cpp` (the `const B=[...]` block), using
  `{{owner}}` where the name goes. This is the single source of truth for the words.
- **Voice clips:** `data/audio/L<hash>.mp3` — one per line, named by a hash of the line.
- **On-device index:** `src/phrasebank.h` — generated; maps each line to its clip file.
- **Generators:** `tools/gen_voice.py`, `tools/gen_phrasebank.py` (+ shared `tools/voicelib.py`).

## One-time setup (creates the edge-tts tool)
```bash
cd tools
python3 -m venv venv
./venv/bin/pip install edge-tts
```
(edge-tts needs internet to generate; clips then live on the device, offline.)

## Make a NEW unit (e.g., a gift for "Sarah", or name-free for selling)
From the project root:
```bash
# 1. Clear the old name's clips (keep the music/ subfolder)
rm -f data/audio/L*.mp3

# 2. Generate clips for the new owner  (OWNER="" = name-agnostic)
OWNER="Sarah" python3 tools/gen_voice.py

# 3. Regenerate the on-device index with the SAME owner
OWNER="Sarah" python3 tools/gen_phrasebank.py     # expect "0 missing clips"

# 4. Build + flash firmware, then load the clips
pio run -t upload
pio run -t uploadfs        # ALWAYS run after a firmware flash (it wipes the FS)
```
`OWNER` must be the same for steps 2 and 3, or the device won't find its clips.

## This first unit
Built with `OWNER="Tasfia"` (the default in `voicelib.py`). Its clips are already
in `data/audio/` — leave them as-is. `gen_phrasebank.py` with OWNER=Tasfia
reproduces the matching `src/phrasebank.h` with 0 missing clips (verified).

## Changing/adding lines
Edit the `{{owner}}` lines in `src/main.cpp`'s `const B=[...]`, then re-run
`gen_voice.py` + `gen_phrasebank.py` (they read straight from main.cpp, so the
dashboard, clips, and device stay in sync automatically).

## Notes
- Stage directions like `*yawn*` / `*happy little sigh*` become soft sounds in the
  audio (handled by `speakify()` in voicelib.py) but keep their text on screen.
- Mood/background music is separate: `tools/gen_music.py` (needs numpy + ffmpeg/lame),
  writes `data/audio/music/music_<mood>.mp3`. Music has no name in it, so it's the
  same for every unit — only regenerate it if you change the moods.
