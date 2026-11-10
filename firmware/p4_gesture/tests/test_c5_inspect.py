"""Offline image checks must verify actual checksums, not just parse headers."""
from pathlib import Path
import sys
import unittest
from esptool.bin_image import ESP32C5FirmwareImage,ImageSegment

sys.path.insert(0,str(Path(__file__).resolve().parents[3]/'tools'))
import c5_inspect as inspection


class InspectTests(unittest.TestCase):
    def image(self):
        x=ESP32C5FirmwareImage();x.chip_id=23;x.entrypoint=0x40800000
        x.min_rev_full=100;x.max_rev_full=199;x.flash_size_freq=0x2f
        x.segments=[ImageSegment(0x40800000,b'\x01'*32)]
        x.segments[0].name='.test'
        return x.save(None)

    def test_valid_image_checked(self):
        b=self.image();r=inspection.inspect_image(b,0,len(b))
        self.assertTrue(r['checksum_ok']);self.assertTrue(r['digest_ok'])
        self.assertEqual(r['chip_id'],23)

    def test_corrupt_digest_rejected(self):
        b=bytearray(self.image());b[-1]^=1
        with self.assertRaisesRegex(ValueError,'Invalid C5'):
            inspection.inspect_image(b,0,len(b))

    def test_corrupt_segment_rejected(self):
        b=bytearray(self.image());b[33]^=1
        with self.assertRaisesRegex(ValueError,'Invalid C5'):
            inspection.inspect_image(b,0,len(b))

    def test_partial_not_a_full_backup(self):
        with self.assertRaisesRegex(ValueError,'complete'):
            inspection.inspect_backup(b'\xff'*0x20000)

    def test_empty_slot_not_claimed_valid(self):
        self.assertFalse(inspection.inspect_image(b'\xff'*4096,0,4096)['present'])


if __name__=='__main__':unittest.main()
