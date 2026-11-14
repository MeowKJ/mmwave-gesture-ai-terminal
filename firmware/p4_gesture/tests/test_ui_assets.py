"""Offline source/license/integrity tests; never fetch packages or touch USB."""
from pathlib import Path
import hashlib
import json
import math
import re
import unittest

MAIN = Path(__file__).resolve().parents[1] / 'main'


class UiAssetsTest(unittest.TestCase):
    def test_lucide_source_hashes_and_license(self):
        root = MAIN / 'assets/icons'
        manifest = json.loads((root/'manifest.json').read_text(encoding='utf-8'))
        self.assertEqual(manifest['version'],'0.468.0')
        self.assertEqual(manifest['license'],'ISC')
        self.assertIn('Lucide Contributors',(root/'LICENSE').read_text())
        for name,spec in manifest['icons'].items():
            raw = (root/f'{name}.svg').read_bytes()
            self.assertEqual(hashlib.sha256(raw).hexdigest(),spec['sha256'])
            self.assertIn(b'viewBox="0 0 24 24"',raw)

    def test_all_native_masks_have_antialiased_edges(self):
        manifest = json.loads((MAIN/'assets/icons/manifest.json').read_text())
        code = (MAIN/'meoave_icons.c').read_text()
        total = 0
        for name,spec in manifest['icons'].items():
            for size in spec['sizes']:
                symbol = name.replace('-','_')+f'_{size}'
                text = re.search(r'pixels_'+symbol+r'\[\] = \{(.*?)\};',code,re.S).group(1)
                pixels = bytes(int(v,16) for v in re.findall(r'0x([0-9a-f]{2})',text))
                self.assertEqual(len(pixels),size*size)
                self.assertGreater(sum(0<v<255 for v in pixels),size)
                self.assertGreater(max(pixels),240)
                self.assertIn(f'kind=={spec["kind"]} && size=={size}',code)
                total += len(pixels)
        self.assertLess(total,40000)  # don't grow a font/runtime vector engine

    def test_music_source_and_checked_in_pcm(self):
        root = MAIN/'assets/audio'
        manifest = json.loads((root/'manifest.json').read_text())
        source = root/'jingles_PIZZI07.ogg'
        self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(),manifest['source_sha256'])
        self.assertEqual(manifest['license'],'CC0-1.0')
        self.assertIn('Kenney',(root/'LICENSE.txt').read_text())
        code = (root/'preview_pcm.inc').read_text()
        frames = int(re.search(r'MEO_PREVIEW_PCM_FRAMES (\d+)u',code).group(1))
        values = [int(v) for v in re.findall(r'-?\d+',code.split('= {',1)[1])]
        self.assertEqual(len(values),frames)
        self.assertEqual(values[0],0)
        self.assertEqual(values[-1],0)
        peak=max(abs(v) for v in values)
        self.assertGreaterEqual(peak,21000)
        self.assertLessEqual(peak,22000)
        self.assertGreater(math.sqrt(sum(v*v for v in values)/frames),5000)
        raw=b''.join(v.to_bytes(2,'little',signed=True) for v in values)
        self.assertEqual(hashlib.sha256(raw).hexdigest(),manifest['pcm_sha256'])
        self.assertEqual(manifest['pcm_peak'],peak)
        self.assertEqual(manifest['pcm_sample_rate'],24000)
        self.assertEqual(manifest['pcm_frames'],frames)
        self.assertGreater(frames,24000)

    def test_native_library_linked_in_firmware_and_desktop(self):
        firmware = (MAIN/'CMakeLists.txt').read_text(encoding='utf-8')
        desktop = (MAIN.parent/'tests/desktop/CMakeLists.txt').read_text(encoding='utf-8')
        self.assertIn('"meoave_icons.c"',firmware)
        self.assertEqual(desktop.count('../../main/meoave_icons.c'),3)


if __name__ == '__main__':
    unittest.main()
