#!/usr/bin/env python3
"""Generate PCM from the vendored CC0 Kenney music source.

Optional generation dependencies: numpy, soundfile. Firmware builds use the
checked-in PCM without these packages, an Ogg decoder or network access.
"""
from pathlib import Path
import hashlib
import json

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'firmware/p4_gesture/main/assets/audio'


def main():
    import numpy as np
    import soundfile as sf
    source = ASSETS / 'jingles_PIZZI07.ogg'
    manifest = json.loads((ASSETS / 'manifest.json').read_text(encoding='utf-8'))
    if hashlib.sha256(source.read_bytes()).hexdigest() != manifest['source_sha256']:
        raise ValueError('Preview source hash mismatch')
    data, rate = sf.read(source, dtype='float64', always_2d=True)
    if not len(data) or not np.isfinite(data).all():
        raise ValueError('Preview source is empty or non-finite')
    mono = data.mean(axis=1)
    target_rate = 24000
    # Windowed sinc anti-alias filter before downsampling, not sample dropping.
    x = np.arange(-48,49)
    cutoff = 10500 / rate
    kernel = 2*cutoff*np.sinc(2*cutoff*x)*np.hamming(len(x))
    kernel /= kernel.sum()
    mono = np.convolve(mono,kernel,mode='same')
    positions = np.arange(round(len(mono)*target_rate/rate))*rate/target_rate
    mono = np.interp(positions,np.arange(len(mono)),mono)
    # The old 7000 peak left 13.4 dB unused even at 100% codec volume. Master
    # THIS fixture, not the user's global gain: ~+10 dB vs the old asset,
    # retaining ~3.5 dB sample headroom. Interpolation cannot overshoot endpoints.
    target_peak=22000
    source_peak=float(abs(mono).max())
    if source_peak<1e-9:
        raise ValueError('Preview source is silent')
    mono *= target_peak/source_peak  # codec still owns volume/mute
    ramp = min(240,len(mono)//2)
    mono[:ramp] *= np.linspace(0,1,ramp)
    mono[-ramp:] *= np.linspace(1,0,ramp)
    pcm = np.rint(mono).astype('<i2')
    peak=int(np.abs(pcm.astype(np.int32)).max())
    rms=float(np.sqrt(np.mean(pcm.astype(np.float64)**2)))
    if not 21000<=peak<=target_peak or not 5000<rms<6500:
        raise ValueError(f'Unexpected mastered music level: peak={peak}, RMS={rms:.1f}')
    lines=['/* Kenney Music Jingles / jingles_PIZZI07, CC0. See LICENSE.txt. */',
           f'#define MEO_PREVIEW_PCM_FRAMES {len(pcm)}u',
           'static const int16_t meo_preview_pcm[MEO_PREVIEW_PCM_FRAMES] = {']
    for i in range(0,len(pcm),20):
        lines.append('    '+','.join(str(v) for v in pcm[i:i+20])+',')
    lines += ['};','']
    (ASSETS / 'preview_pcm.inc').write_text('\n'.join(lines),encoding='utf-8')
    manifest.update(generation=f'tools/build_preview_audio.py; mono 24kHz PCM16, peak {target_peak}, 10ms edge fades',
                    pcm_sha256=hashlib.sha256(pcm.tobytes()).hexdigest(),
                    pcm_sample_rate=target_rate,pcm_frames=len(pcm),pcm_peak=peak,
                    pcm_rms=round(rms,3))
    manifest['generator'].update(soundfile=sf.__version__,numpy=np.__version__)
    (ASSETS/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(f'Generated {len(pcm)*2} PCM bytes, {len(pcm)/target_rate:.3f} s, peak={peak}, RMS={rms:.1f}')


if __name__ == '__main__':
    main()
