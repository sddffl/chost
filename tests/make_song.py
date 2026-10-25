"""Synthesises a 16 second demo song and its parts, for screenshots and the stem separation self test.

    python3 tests/make_song.py <folder>

Writes mysong.wav (the mix) and one file per part, named like the stems of tools/chost_separate.py
(vocals, drums, bass, guitar, piano, other, kick, snare, toms, hh, ride, crash, lead, backing).
"""
import os
import sys
import wave

import numpy as np
sr=44100; bpm=120; beat=60/bpm; bars=8; n=int(sr*beat*4*bars)
t=np.arange(n)/sr
out=sys.argv[1]
os.makedirs(out, exist_ok=True)
rng=np.random.default_rng(1)
def env(length, a=0.005, d=0.2):
    x=np.arange(length)/sr
    return np.minimum(1, x/a)*np.exp(-x/d)
def place(buf, sig, at):
    i=int(at*sr); j=min(n, i+len(sig)); buf[i:j]+=sig[:j-i]
kick=np.zeros(n); snare=np.zeros(n); hh=np.zeros(n); toms=np.zeros(n); crash=np.zeros(n); ride=np.zeros(n)
L=int(0.4*sr); x=np.arange(L)/sr
k=np.sin(2*np.pi*(50+120*np.exp(-x*30))*x)*env(L,0.002,0.15)
s=(rng.standard_normal(L)*0.6+np.sin(2*np.pi*190*x)*0.5)*env(L,0.001,0.08)
h=rng.standard_normal(int(0.08*sr)); h=np.diff(h,prepend=0)*env(len(h),0.001,0.02)
cr=np.diff(rng.standard_normal(int(1.5*sr)),prepend=0)*env(int(1.5*sr),0.002,0.5)
for b in range(bars*4):
    at=b*beat
    if b%4 in (0,2) or (b%8==7): place(kick,k,at)
    if b%4 in (1,3): place(snare,s,at)
    for e in range(2): place(hh,h*(0.7 if e else 1),at+e*beat/2)
    if bars*4-4<=b: 
        tl=int(0.3*sr); xx=np.arange(tl)/sr
        place(toms,np.sin(2*np.pi*(140-20*(b%4))*xx)*env(tl,0.002,0.12),at+beat/2)
place(crash,cr,0); place(crash,cr,beat*16)
for b in range(16,32): place(ride, np.sin(2*np.pi*3100*x[:int(0.3*sr)])*env(int(0.3*sr),0.001,0.1)*0.3, b*beat)
chords=[[57,60,64],[53,57,60],[48,52,55],[55,59,62]]
f=lambda m:440*2**((m-69)/12)
bass=np.zeros(n); piano=np.zeros(n); guitar=np.zeros(n); lead=np.zeros(n); backing=np.zeros(n)
for bar in range(bars):
    ch=chords[bar%4]; at=bar*4*beat
    for q in range(8):
        Lb=int(beat/2*sr); xb=np.arange(Lb)/sr
        fr=f(ch[0]-24)
        place(bass,(np.sin(2*np.pi*fr*xb)+0.3*np.sign(np.sin(2*np.pi*fr*xb)))*env(Lb,0.003,0.25),at+q*beat/2)
    Lp=int(4*beat*sr); xp=np.arange(Lp)/sr
    for m in ch:
        place(piano,sum(np.sin(2*np.pi*f(m)*h_*xp)/h_**1.5 for h_ in (1,2,3))*env(Lp,0.004,1.2)*0.5,at)
    for q in range(4):
        Lg=int(beat*sr); xg=np.arange(Lg)/sr
        for i_,m in enumerate(ch):
            fr=f(m+12)
            place(guitar,np.sign(np.sin(2*np.pi*fr*xg))*0.25*env(Lg,0.002,0.18),at+q*beat+i_*0.012)
mel=[69,71,72,76,74,72,71,69,67,69,71,72,74,72,71,67]
for i,m in enumerate(mel*2):
    at=i*beat*1.0
    if at>=bars*4*beat or i%8==7: continue
    Lv=int(beat*0.95*sr); xv=np.arange(Lv)/sr
    vib=1+0.006*np.sin(2*np.pi*5.5*xv)
    ph=2*np.cumsum(np.pi*f(m)*vib)/sr
    v=(np.sin(ph)+0.4*np.sin(2*ph)+0.2*np.sin(3*ph))*np.minimum(1,xv/0.05)*np.minimum(1,(xv[-1]-xv)/0.08)
    place(lead,v*0.5,at)
    if bars*2*beat<=at: place(backing,np.sin(ph*1.26)*0.25*np.minimum(1,xv/0.08)*np.minimum(1,(xv[-1]-xv)/0.1),at)
parts={'kick':kick*0.8,'snare':snare*0.5,'hh':hh*0.25,'toms':toms*0.5,'ride':ride,'crash':crash*0.15,'bass':bass*0.35,'piano':piano*0.25,'guitar':guitar*0.3,'lead':lead*0.5,'backing':backing*0.6}
mix=sum(parts.values())
def write(name,sig):
    sig=np.clip(sig,-1,1); st=np.stack([sig,sig],1)
    with wave.open(os.path.join(out,name+'.wav'),'wb') as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(sr); w.writeframes((st*32000).astype('<i2').tobytes())
peak=np.abs(mix).max(); g=0.9/peak
for k_,v in parts.items(): write(k_,v*g)
write('drums',sum(parts[k] for k in ['kick','snare','hh','toms','ride','crash'])*g)
write('vocals',(lead+backing)*g*0.55)
write('other',np.zeros(n))
write('mysong',mix*g)

