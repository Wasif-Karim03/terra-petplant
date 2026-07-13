# voicelib.py — shared helpers so the voice clips AND src/phrasebank.h always
# match. Both gen_voice.py and gen_phrasebank.py import this, so the clip
# filenames (a hash of each line) are guaranteed identical. DO NOT change the
# hashing/normalize logic or old clips stop matching.
import re, os

# The voice character — keep these fixed so EVERY unit sounds the same.
VOICE = "en-US-AnaNeural"   # edge-tts cute young-girl voice
RATE  = "-8%"               # slightly slower = cozy
OWNER_DEFAULT = "Tasfia"    # this first unit; override with OWNER env var

EMOJI = re.compile(r'[\U0001F000-\U0001FAFF☀-➿️*_~]')

def apply_owner(text, owner):
    """Substitute {{owner}}. Empty owner -> gracefully drop the name."""
    if owner:
        return text.replace("{{owner}}", owner)
    t = re.sub(r',\s*\{\{owner\}\}', '', text)   # "thank you, {{owner}}." -> "thank you."
    t = re.sub(r'\{\{owner\}\},?\s*', '', t)      # "{{owner}}... I'm" -> "... I'm"
    t = re.sub(r'\s+([.!?,…])', r'\1', t)          # fix space before punctuation
    return re.sub(r'\s{2,}', ' ', t).strip()

def normalize(t):
    return re.sub(r'\s+', ' ', EMOJI.sub('', t)).strip()

def line_id(text):   # djb2 -> base36; must match the dashboard's lineId()
    h = 5381
    for ch in text:
        h = ((h * 33) + ord(ch)) & 0xFFFFFFFF
    if h == 0:
        return "L0"
    digs = "0123456789abcdefghijklmnopqrstuvwxyz"; s = ""
    while h:
        s = digs[h % 36] + s; h //= 36
    return "L" + s

def clip_id(raw_line, owner):
    """The clip filename stem for a raw {{owner}} line, for a given owner."""
    return line_id(normalize(apply_owner(raw_line, owner)))

# Stage directions -> soft vocal sounds in the AUDIO (id is still from the text).
STAGE = {"*happy little sigh*": "ahh,", "*yawn*": "haaaah,",
         "*leans into your hand*": "", "*stretches*": ""}
def speakify(raw_line, owner):
    """What edge-tts should actually say for a line."""
    t = apply_owner(raw_line, owner)
    for k, v in STAGE.items():
        t = t.replace(k, v)
    t = re.sub(r'\*[^*]*\*', '', t)      # drop any other stage directions
    return re.sub(r'\s+', ' ', EMOJI.sub('', t)).strip()

def project_root():
    return os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

def extract_lines(main_cpp_path):
    """Pull the raw {{owner}} line strings from the phrasebank in main.cpp."""
    src = open(main_cpp_path, encoding="utf-8").read()
    def block(a, b):
        i = src.find(a)
        if i < 0: return ""
        j = src.find(b, i)
        return src[i:j] if j > 0 else src[i:]
    regions = [
        block("const B=[", "];\n  const ASIDE"),
        block("const ASIDE=[", "];\n  function pick"),
        block("const AMB=[", "];"),
        '"Oh — you can hear me now! Hi there, {{owner}}!"',
    ]
    raw = []
    for r in regions:
        raw += re.findall(r'"([^"]*)"', r)
    return [t for t in raw if "{{ms}}" not in t]   # drop the dynamic milestone slot
