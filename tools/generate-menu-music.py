#!/usr/bin/env python3
"""Synthesize an original smooth-jazz loop without recordings or external samples."""
import argparse
from array import array
import math
from pathlib import Path
import random
import struct
import wave

RATE = 22050
BPM = 96
BEAT = 60 / BPM
FRAMES = int(RATE * BEAT * 32)  # Eight bars, 20 seconds; quad-byte aligned.


def chunk(name, data):
    return name + struct.pack('>I', len(data)) + data + (b'\0' if len(data) & 1 else b'')


def render():
    rng = random.Random(3096)
    mix = array('d', [0.0]) * FRAMES

    def note(beat, duration, midi, level, voice):
        frequency = 440 * 2 ** ((midi-69)/12)
        start = round(beat * BEAT * RATE)
        count = round(duration * BEAT * RATE)
        seconds = count / RATE
        phase = rng.random() * math.tau
        for i in range(count):
            t = i / RATE
            attack = min(1, t / (0.045 if voice == 'horn' else 0.012))
            release = min(1, (seconds-t)/0.08)
            p = math.tau * frequency * t
            if voice == 'piano':
                envelope = math.exp(-t*2.5)
                value = math.sin(p + 1.7*math.exp(-t*7)*math.sin(p*2))
                value += 0.15*math.sin(p*2)*math.exp(-t*5)
            elif voice == 'bass':
                envelope = math.exp(-t*2)
                value = math.sin(p) + 0.24*math.sin(p*2)*math.exp(-t*6)
            else:
                envelope = 0.85 + 0.15*math.sin(t*9)
                vibrato = 0.025*min(1,t/0.3)*math.sin(math.tau*5*t)
                value = math.sin(p+vibrato) + 0.22*math.sin(p*2+phase) + 0.07*math.sin(p*3)
            mix[(start+i) % FRAMES] += level * attack * release * envelope * value

    def percussion(beat, kind, level):
        start = round(beat*BEAT*RATE)
        seconds = 0.18 if kind == 'kick' else (0.22 if kind == 'brush' else 0.07)
        low = previous = 0.0
        for i in range(round(seconds*RATE)):
            t = i/RATE
            noise = rng.uniform(-1,1)
            low += 0.18*(noise-low)
            if kind == 'kick':
                value = math.sin(math.tau*(49*t + 1.4*(1-math.exp(-t*30))))*math.exp(-t*26)
            elif kind == 'brush':
                value = (0.6*low+0.4*noise)*math.exp(-t*18)
            else:
                value = (noise-previous)*math.exp(-t*60)
            previous = noise
            mix[(start+i) % FRAMES] += value*level*min(1,t/0.003)

    chords = [(48, [64,67,71,74]), (45, [60,64,67,71]),
              (50, [60,64,65,69]), (43, [59,64,65,69])]
    for bar in range(8):
        base=bar*4
        root, chord=chords[bar%4]
        for beat, duration, level in [(0,1.8,0.11),(1.66,1.1,0.065),(3,1.15,0.075)]:
            for index, pitch in enumerate(chord):
                note(base+beat+index*0.012,duration,pitch,level,'piano')
        for beat,pitch in [(0,root),(1.66,root+7),(2.5,root+12),(3.5,root+7)]:
            note(base+beat,0.65,pitch,0.27,'bass')
        for beat in (0,2.5): percussion(base+beat,'kick',0.08)
        for beat in (1,3): percussion(base+beat,'brush',0.055)
        for beat in range(4):
            percussion(base+beat,'hat',0.025)
            percussion(base+beat+0.66,'hat',0.016)
    # Original lead phrases with space between them; no existing tune is quoted.
    melody = [(0.66,0.6,76),(1.5,0.8,74),(2.66,0.9,71),
              (4.5,0.6,72),(5.33,0.6,71),(6.33,1.1,69),
              (8.66,0.6,69),(9.5,0.7,72),(10.66,0.8,76),
              (12.5,0.65,74),(13.5,0.65,71),(14.66,0.8,69),
              (16.66,0.6,79),(17.5,0.6,76),(18.66,0.8,74),
              (20.5,0.6,72),(21.33,0.6,71),(22.33,1.0,69),
              (24.66,0.6,72),(25.5,0.7,76),(26.66,0.8,77),
              (28.5,0.6,76),(29.33,0.6,74),(30.33,0.9,71)]
    for beat,duration,pitch in melody: note(beat,duration,pitch,0.16,'horn')
    # Circular early reflections retain tails across the loop boundary.
    dry=array('d',mix)
    for delay,level in [(0.087,0.12),(0.179,0.075),(0.293,0.04)]:
        offset=round(delay*RATE)
        for i in range(FRAMES): mix[i] += dry[(i-offset)%FRAMES]*level
    mean=sum(mix)/FRAMES
    peak=max(abs(value-mean) for value in mix)
    return [(value-mean)*0.66/peak for value in mix]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=Path(__file__).resolve().parent.parent/'assets/menu-music.aiff')
    parser.add_argument('--wav',type=Path,help='Optional 16-bit WAV listening copy')
    args=parser.parse_args()
    if args.output.suffix.lower() not in ('.aiff','.aif'):
        parser.error('Choose an AIFF output filename')
    if args.wav and (args.wav.suffix.lower()!='.wav' or args.wav.resolve()==args.output.resolve()):
        parser.error('Choose a separate .wav listening-copy filename')
    samples=render()
    rng=random.Random(9603)
    pcm=bytes(round(max(-128,min(127,value*127+(rng.random()-rng.random())*0.5))) & 255 for value in samples)
    # AIFF uses signed 8-bit PCM. 22050 as an IEEE 80-bit extended rate.
    comm=struct.pack('>hIh',1,FRAMES,8)+bytes.fromhex('400dac44000000000000')
    body=b'AIFF'+chunk(b'COMM',comm)+chunk(b'SSND',b'\0'*8+pcm)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_bytes(b'FORM'+struct.pack('>I',len(body))+body)
    if args.wav:
        args.wav.parent.mkdir(parents=True,exist_ok=True)
        with wave.open(str(args.wav),'wb') as output:
            output.setnchannels(1); output.setsampwidth(2); output.setframerate(RATE)
            output.writeframes(b''.join(struct.pack('<h',(s if s<128 else s-256)*256) for s in pcm))
    print(f'Created {args.output}: original 20-second jazz loop, 96 BPM, mono 22050 Hz, 8-bit PCM')


if __name__=='__main__': main()
