#!/usr/bin/env python3
# gen_terra_voice.py — generate the NEW "Terra" (succulent) voice set into
# data/audio2/  (the old data/audio/ set is left untouched).
# Same voice + softness as before: en-US-AnaNeural, rate -8%.
# [SFX: ...] cues -> soft vocal sounds (edge-tts is a plain engine).
import os, sys, subprocess, json, re
sys.path.insert(0, os.path.dirname(__file__))
import voicelib as V

PROJ = V.project_root()
OUT  = os.path.join(PROJ, "data", "audio")       # flashed dir (Terra clips + music/)
PBH  = os.path.join(PROJ, "src", "phrasebank.h") # generated bucket table for the firmware
TTS  = os.environ.get("EDGE_TTS",
        "/private/tmp/claude-501/-Users-wasifkarim-Claude-Projects-Robotics-PetPlant/"
        "a16aee0b-3a2e-40d0-a460-d20931aca9c8/scratchpad/ttsvenv/bin/edge-tts")
os.makedirs(OUT, exist_ok=True)

# [SFX: ...] -> soft sound (keeps the gentle feel on a plain TTS engine)
SFX = {
    "[SFX: yawn]": "haaaah,", "[SFX: stretch]": "", "[SFX: soft breath]": "mmm,",
    "[SFX: soft exhale]": "ahh,", "[SFX: soft hum]": "mmm,",
}
def speakify(line):
    t = line
    for k, v in SFX.items():
        t = t.replace(k, v)
    t = re.sub(r'\[SFX:[^\]]*\]', '', t)         # any other SFX -> drop
    t = V.EMOJI.sub('', t)
    return re.sub(r'\s+', ' ', t).strip()

# ---- Terra v3 phrasebank (Tasfia written inline where wanted) ----
B = {
 "intro": ["Oh… hello. I'm Terra. I think I'm yours now.",
           "There's light — good. I'm Terra. Nice to meet you, Tasfia.",
           "[SFX: soft breath] So this is home. I'm Terra. I don't need much… just you, mostly.",
           "I'm awake. I'm Terra — fair warning, I'm a low-drama sort of plant."],
 "welcome": ["Oh — you're back. I was fine, but it's better with you here.",
             "[SFX: stretch] …mmm. There you are.",
             "Hi again. Good to see your face, Tasfia.",
             "Back together. Held down the windowsill while you were out.",
             "Bright day out there. Glad you're in."],
 "just_watered": ["Ahh… that's the one. That'll last me a good while.",
                  "Mmm. Deep drink. Now let me dry all the way out and I'm set.",
                  "[SFX: soft exhale] …perfect. You won't need to do that again for a couple weeks.",
                  "Oh, that's good. Right to the roots. Thank you, Tasfia.",
                  "Just enough. I'm a bit of a camel — that goes a long way."],
 "thirst_l1": ["Soil's finally gone dry. A drink sometime this week, maybe.",
               "Getting a little light on water. No rush at all.",
               "Think I'm ready for a soak whenever you are."],
 "thirst_l2": ["Okay — I'm actually thirsty now. Leaves are going a bit soft.",
               "It's been a while since my last drink, Tasfia. Could use one.",
               "Starting to wrinkle a little. A good soak would set me right."],
 "overwater": ["Oof… my soil's still wet. Let's hold off — I don't like staying damp.",
               "Roots are soggy, Tasfia. Please let me dry all the way out before the next drink.",
               "Mm — that's too much. Soggy feet make me nervous. Give me a week to dry?",
               "Still wet down here. I'd honestly rather be thirsty than waterlogged.",
               "Let's pause the watering. Wet-for-too-long is the one thing that really gets to me."],
 "craving_light": ["It's been dim a few days now. I could use more light, Tasfia.",
                   "Feeling a bit starved for sun. A brighter spot would do me good.",
                   "Not much light lately… I'll start stretching if this keeps up.",
                   "Could I sit somewhere sunnier? I'm a sun kid at heart."],
 "basking": ["Mmm. Good light today. Exactly what I want.",
             "Soaking up every bit of this sun. Perfect.",
             "Bright and warm — my favorite. Don't mind me, just photosynthesizing."],
 "cold": ["Brr… it's cold. I don't do frost well, Tasfia — somewhere warmer?",
          "Getting chilly. Cold's the thing I actually worry about.",
          "It's dropping in here. Could we move away from the draft?",
          "If it's near freezing, I could really get hurt. A warmer corner, please?"],
 "hot": ["Warm one today. Doesn't bother me much — I'm built for this.",
         "Toasty. I'm fine… though if that sun's blazing, watch my leaves don't scorch.",
         "Hot out. Honestly? This is kind of my element."],
 "humid": ["Air's a bit muggy today. I like things drier, honestly.",
           "Feels damp in here. Not really my climate, Tasfia.",
           "Bit sticky today. Some airflow would be lovely."],
 "dry_content": ["Nice and dry today. Just how I like it.",
                 "Lovely dry air. A desert kid's happy place."],
 "morning": ["Morning. First light's the best — soak it up with me?",
             "Good morning, Tasfia. Sleep alright?",
             "New day. Give me some sun and I'm easy to please.",
             "Mmm, morning. Light's just coming in."],
 "evening": ["Light's going soft. Winding down over here.",
             "Evening already… gentle day, that.",
             "Sun's down. I'll rest and grow a little overnight.",
             "Quiet evening. I like these."],
 "night": ["[SFX: yawn] …goodnight, Tasfia. I'll be right here.",
           "It's late. Rest well.", "Sleepy time. Sweet dreams.",
           "Shh… I've got the night shift. Sleep, Tasfia."],
 "content": ["Feeling good. Fed, dry, and lit — nothing more I need.",
             "Just soaking up the afternoon.",
             "Everything's about right. Happy little plant over here.",
             "Comfortable. Nothing to report — in the best way."],
 "musing": ["Mmm… nice and quiet.", "Just being a plant. Low-maintenance life suits me.",
            "Growing a little every day, quietly.", "It's nice, having you around.",
            "I like it here. Sunny corner, good company."],
 "interaction": ["Oh — hi.", "[SFX: soft hum] …careful of my leaves, but that's nice.",
                 "Hey, you. Hello, Tasfia.", "Mmm, hello. Wasn't expecting that."],
 "wx_clear": ["Clear and bright out there. My kind of day.",
              "Sun's out. I could bask in this all afternoon."],
 "wx_warmdry": ["Warm and dry out. Feels like home, honestly."],
 "wx_rain": ["Rain again. Cozy for you — I'll just enjoy staying dry in here.",
             "Wet out there. Glad my roots are safe and dry, Tasfia."],
 "wx_storm": ["Storm outside. We're snug. My soil stays dry — that's all I ask."],
 "wx_cloudy": ["Grey out. If it stays this dim a few days, I might get peckish for light."],
 "wx_snow": ["Snow out there. Keep me warm and away from that cold window, would you?",
             "Freezing outside. This is the weather I hide from — glad I'm indoors."],
 "wx_heatwave": ["Scorcher out there. Doesn't faze me — desert blood."],
 "wx_windy": ["Breezy today. I actually like the airflow — keeps me healthy."],
 "light_on": ["Oh — light's on. You're home.", "There's the light. Evening, then. Hi, Tasfia."],
 "light_late": ["Up late? I'll keep you company."],
 "dark_long": ["It's been dark in here a while… and I could use the light, honestly."],
 "welcome_back_long": ["You were gone a while. Barely noticed — I'm a camel. But I did miss you.",
                       "There you are — a few days, that. All fine here, Tasfia.",
                       "Back at last. Told you I'd be okay on my own… still glad you're here."],
 "senses_fuzzy": ["I can't quite feel my surroundings right now…",
                  "My senses are a little fuzzy — are my wires okay, Tasfia?",
                  "Feeling a bit disconnected today. Bear with me.",
                  "Something's gone quiet in me… I can't read the room."],
 "milestone_1":  ["One day together. Easy start."],
 "milestone_3":  ["Three days in. Settling nicely — haven't needed a thing."],
 "milestone_7":  ["A week with you. Still plump, still happy."],
 "milestone_14": ["Two weeks — about time for a drink, actually. Feels like home now, Tasfia."],
 "milestone_30": ["A month together. I've asked for almost nothing and gotten everything. Thank you — I mean it."],
}

# bucket -> face mood (valid: neutral happy love sleepy thirsty cold hot sad)
# and a nominal priority (documentation; real ordering lives in vPickNeed()).
MOOD = {
 "intro":"love","welcome":"love","welcome_back_long":"love","just_watered":"happy",
 "thirst_l1":"thirsty","thirst_l2":"thirsty","overwater":"sad","craving_light":"sad",
 "basking":"happy","cold":"cold","hot":"hot","humid":"sad","dry_content":"happy",
 "morning":"happy","evening":"sleepy","night":"sleepy","content":"happy","musing":"neutral",
 "interaction":"love","wx_clear":"happy","wx_warmdry":"happy","wx_rain":"neutral",
 "wx_storm":"neutral","wx_cloudy":"neutral","wx_snow":"cold","wx_heatwave":"hot",
 "wx_windy":"neutral","light_on":"love","light_late":"love","dark_long":"sad",
 "senses_fuzzy":"sad","milestone_1":"love","milestone_3":"love","milestone_7":"love",
 "milestone_14":"love","milestone_30":"love",
}
PRIO = {  # frost/rot emergencies highest; ambient lowest
 "intro":130,"welcome":120,"welcome_back_long":118,"interaction":115,"just_watered":110,
 "cold":100,"overwater":95,"thirst_l2":88,"hot":80,"craving_light":74,"thirst_l1":66,
 "humid":58,"senses_fuzzy":52,"morning":50,"evening":48,"night":46,"dark_long":44,
 "milestone_30":42,"milestone_14":42,"milestone_7":42,"milestone_3":42,"milestone_1":42,
 "light_on":40,"light_late":40,"wx_storm":34,"wx_snow":34,"wx_heatwave":32,"wx_rain":30,
 "wx_windy":28,"wx_cloudy":26,"wx_clear":24,"wx_warmdry":24,"dry_content":22,"basking":22,
 "content":20,"musing":10,
}
def cesc(s):  # escape for a C string literal
    return s.replace("\\", "\\\\").replace('"', '\\"')

if not os.path.exists(TTS):
    sys.exit(f"edge-tts not found at {TTS} (set EDGE_TTS=...)")

manifest = {}
buckets  = {}                 # ordered: bucket -> [(clip_path, display), ...]
made = existed = fail = 0
for bucket, lines in B.items():
    buckets.setdefault(bucket, [])
    for line in lines:
        spoken = speakify(line)
        if not spoken:
            continue
        lid  = V.line_id(V.normalize(spoken))
        path = os.path.join(OUT, lid + ".mp3")
        buckets[bucket].append(("/audio/" + lid + ".mp3", line))
        manifest[lid] = {"bucket": bucket, "display": line, "spoken": spoken}
        if os.path.exists(path) and os.path.getsize(path) > 0:
            existed += 1; continue
        r = subprocess.run([TTS, f"--voice={V.VOICE}", f"--rate={V.RATE}",
                            f"--text={spoken}", f"--write-media={path}"], capture_output=True)
        if r.returncode == 0 and os.path.exists(path):
            made += 1; print(f"  + [{bucket}] {spoken[:50]}")
        else:
            fail += 1; print(f"  ! FAIL {lid}: {r.stderr.decode()[:100]}")

# ---- emit src/phrasebank.h (Terra buckets, moods, clip filenames) ----
out = ["// AUTO-GENERATED by tools/gen_terra_voice.py — Terra (succulent). Do not edit by hand.",
       "#pragma once",
       "struct PBLine { const char* clip; const char* text; };",
       "struct PBBucket { const char* id; uint8_t prio; const char* mood; const PBLine* lines; uint8_t n; };",
       ""]
for bucket, items in buckets.items():
    out.append(f"static const PBLine PB_{bucket}[] = {{")
    for clip, disp in items:
        out.append(f'  {{ "{clip}", "{cesc(disp)}" }},')
    out.append("};")
out.append("")
out.append("static const PBBucket PHRASEBANK[] = {")
for bucket, items in buckets.items():
    out.append(f'  {{ "{bucket}", {PRIO.get(bucket,20)}, "{MOOD.get(bucket,"neutral")}", '
               f"PB_{bucket}, {len(items)} }},")
out.append("};")
out.append(f"static const int PHRASEBANK_N = {len(buckets)};")
open(PBH, "w").write("\n".join(out) + "\n")

json.dump(manifest, open(os.path.join(PROJ, "tools", "terra_manifest.json"), "w"),
          ensure_ascii=False, indent=1)
print(f"\nTerra voices -> {OUT}\n  {sum(len(v) for v in buckets.values())} lines in "
      f"{len(buckets)} buckets · {made} made, {existed} existed, {fail} failed")
print(f"phrasebank    -> {PBH}")
