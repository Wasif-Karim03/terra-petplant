#!/usr/bin/env python3
# gen_music.py — synthesize the seamless mood-music loops into data/audio/music/.
# Music has no name in it, so it's identical for every unit. Only re-run if you
# change the moods. Needs numpy + ffmpeg/lame installed.
#   python3 tools/gen_music.py
import numpy as np, subprocess, os, wave, sys
sys.path.insert(0, os.path.dirname(__file__))
import voicelib as V

SR  = 22050
OUT = os.path.join(V.project_root(), "data", "audio", "music")
os.makedirs(OUT, exist_ok=True)

# per-mood: pad drone chord + pentatonic scale (random over it = always pleasant)
M = {
 'happy':   dict(pad=[130.81,164.81,196.00], scale=[523.25,587.33,659.25,783.99,880.00], bpm=96, wave='triangle', cut=1900, vol=0.85),
 'love':    dict(pad=[130.81,174.61,196.00], scale=[349.23,392.00,440.00,523.25,587.33], bpm=70, wave='sine',     cut=1400, vol=0.85),
 'neutral': dict(pad=[130.81,196.00,261.63], scale=[523.25,587.33,659.25,783.99],        bpm=60, wave='sine',     cut=1200, vol=0.7),
 'sleepy':  dict(pad=[ 98.00,130.81,146.83], scale=[261.63,293.66,329.63,392.00],        bpm=46, wave='sine',     cut=680,  vol=0.62),
 'thirsty': dict(pad=[110.00,164.81,220.00], scale=[440.00,523.25,587.33,659.25,783.99], bpm=64, wave='triangle', cut=1100, vol=0.7),
 'cold':    dict(pad=[164.81,246.94,329.63], scale=[659.25,783.99,880.00,987.77,1174.7], bpm=56, wave='sine',     cut=2800, vol=0.62),
 'hot':     dict(pad=[ 87.31,116.54,130.81], scale=[349.23,392.00,440.00,523.25],        bpm=46, wave='sine',     cut=620,  vol=0.62),
 'sad':     dict(pad=[110.00,146.83,174.61], scale=[293.66,349.23,392.00,440.00,523.25], bpm=52, wave='sine',     cut=900,  vol=0.7),
 'drinking':dict(pad=[196.00,261.63,329.63], scale=[523.25,659.25,783.99,1046.5],        bpm=138,wave='triangle', cut=2300, vol=0.9),
}

def wave_fn(f, t, kind):
    ph = 2*np.pi*f*t
    return (2/np.pi)*np.arcsin(np.sin(ph)) if kind == 'triangle' else np.sin(ph)

def lowpass(x, cut):
    n = len(x); X = np.fft.rfft(x); fr = np.fft.rfftfreq(n, 1/SR)
    return np.fft.irfft(X / np.sqrt(1 + (fr/cut)**4), n)

def reverb(x, decay=0.45, wet=0.30):
    irn = int(SR*decay)
    ir = np.random.RandomState(7).randn(irn) * np.exp(-np.linspace(0,5,irn)); ir /= np.max(np.abs(ir))
    n = len(x)+len(ir)-1; nf = 1 << (n-1).bit_length()
    y = np.fft.irfft(np.fft.rfft(x,nf)*np.fft.rfft(ir,nf), nf)[:len(x)]; y /= (np.max(np.abs(y))+1e-9)
    return (1-wet)*x + wet*y

def synth(name, m):
    beat = 60.0/m['bpm']; beats = max(16, int(round(18.0/beat)))
    L = beats*beat; total = L + 1.6; n = int(total*SR); t = np.arange(n)/SR
    rng = np.random.RandomState(abs(hash(name)) % (2**31))
    pad = np.zeros(n)
    for f in m['pad']:
        for d in (-5,0,5): pad += np.sin(2*np.pi*(f+d*0.03*f/100)*t)
    pad /= (len(m['pad'])*3); pad *= (0.75+0.25*np.sin(2*np.pi*(1.0/L)*t))*0.5
    mel = np.zeros(n)
    for b in range(beats):
        if rng.random() < 0.6:
            f = m['scale'][rng.randint(len(m['scale']))]*(0.5 if rng.random()<0.18 else 1)
            dur = beat*(2 if rng.random()<0.3 else 1)*0.95; st=b*beat
            i0=int(st*SR); i1=min(n,int((st+dur+0.1)*SR)); tt=np.arange(i1-i0)/SR
            env = np.minimum(tt/0.04,1.0)*np.exp(-tt/(dur*0.6))
            mel[i0:i1] += wave_fn(f,tt,m['wave'])*env*0.9
    mix = reverb(lowpass(pad+mel*0.55, m['cut']))
    out = mix[:int(L*SR)].copy(); xn=int(0.45*SR); a=np.linspace(0,1,xn)
    tail = mix[int(L*SR):int(L*SR)+xn]
    if len(tail)==xn: out[:xn] = out[:xn]*np.sqrt(a) + tail*np.sqrt(1-a)
    out /= (np.max(np.abs(out))+1e-9); out *= 0.85*m['vol']
    wav = os.path.join(OUT, f"music_{name}.wav")
    with wave.open(wav,'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes((np.clip(out,-1,1)*32767).astype('<i2').tobytes())
    mp3 = os.path.join(OUT, f"music_{name}.mp3")
    subprocess.run(["lame","-q","2","-b","64","--silent",wav,mp3], check=True); os.remove(wav)
    return os.path.getsize(mp3)

print("rendering mood loops ->", OUT)
tot = 0
for name, m in M.items():
    sz = synth(name, m); tot += sz; print(f"  music_{name}.mp3  {sz//1024} KB")
print(f"total {tot//1024} KB")
