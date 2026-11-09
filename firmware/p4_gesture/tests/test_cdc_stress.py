from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from stress_usb_cdc import classify

UI = ('MEO:UI page=eyes frames=10 rotation=180 brightness=80 reduced=0 '
      'running=0 mode=GUIDED elapsed_ms=0 wifi=UNAVAILABLE radar=0 app_view=GRID eye=0 eye_auto=1')


class RecordClassifierTests(unittest.TestCase):
    def test_complete_ui(self):
        self.assertEqual(classify(UI), 'ui')

    def test_expression_fields_are_strict(self):
        for eye in range(7):
            self.assertEqual(classify(UI.replace('eye=0', f'eye={eye}')), 'ui')
        for bad in ('eye=7', 'eye=-1', 'eye=0.5'):
            self.assertEqual(classify(UI.replace('eye=0', bad)), 'malformed')
        self.assertEqual(classify(UI.replace('eye_auto=1', 'eye_auto=2')), 'malformed')

    def test_prefix_suffix_and_concatenation_are_not_salvaged(self):
        for line in (UI[:-2], 'MEO:UI' + UI, UI + 'FLS2:STATUS', 'junk' + UI,
                     'FLS2:STATUS profile=meoave_li', 'EO:UI page=eyes', '\ufffd' + UI):
            with self.subTest(line=line):
                self.assertEqual(classify(line), 'malformed')

    def test_corrupt_machine_record_is_not_ignored_as_log(self):
        self.assertEqual(classify('FLS2:TOUCH_IO driver_ok=10 driver_err=1'), 'malformed')
        self.assertEqual(classify('I (10) meoave: info'), 'log')
        self.assertEqual(classify('I (10) FLS2: profile created'), 'log')
        self.assertEqual(classify('I (10) meoave: truncated ' + UI), 'malformed')

    def test_raw_touch_length_is_exact(self):
        self.assertEqual(classify('FLS2:TOUCH_RAW' + ' 00' * 15), 'touch_raw')
        self.assertEqual(classify('FLS2:TOUCH_RAW' + ' 00' * 14), 'malformed')


if __name__ == '__main__':
    unittest.main()
