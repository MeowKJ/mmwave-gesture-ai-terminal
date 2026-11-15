"""Prevent retired demo entry points from returning to the product build.

These are source-surface checks, not acoustic or physical-screen acceptance.
The desktop C tests separately exercise the real PCM/sentence workers.
"""
from pathlib import Path
import unittest


P4 = Path(__file__).resolve().parents[1]
ROOT = P4.parents[1]
MAIN = P4 / "main"


class ProductionSurfaceTests(unittest.TestCase):
    def test_retired_sources_and_fixture_tools_are_removed(self):
        for relative in (
            "main/main.c", "main/demo_ui.c", "main/demo_ui.h",
            "main/bsp_display.c", "main/bsp_display.h",
            "main/bsp_touch.c", "main/bsp_touch.h",
            "main/bsp_radar.c", "main/bsp_radar.h",
            "main/meoave_conversation.c", "main/meoave_conversation.h",
        ):
            with self.subTest(path=relative):
                self.assertFalse((P4 / relative).exists())
        for name in ("build_voice_fixture.py", "voice_acceptance.py"):
            with self.subTest(tool=name):
                self.assertFalse((ROOT / "tools" / name).exists())

    def test_build_cannot_fall_back_to_legacy_demo(self):
        cmake = (MAIN / "CMakeLists.txt").read_text(encoding="utf-8")
        for filename in ("demo_ui.c", "meoave_conversation.c", "bsp_display.c",
                         "bsp_touch.c", "bsp_radar.c", "voice_fixture"):
            self.assertNotIn(filename, cmake)
        for filename in ("fls2_official_source.c", "fls2_official_board.c",
                         "meoave_ui.c", "meoave_voice.c", "meoave_voice_link.c"):
            self.assertIn('"' + filename + '"', cmake)
        self.assertIn("if(CMAKE_BUILD_EARLY_EXPANSION)", cmake)
        self.assertIn("message(FATAL_ERROR", cmake)

    def test_product_commands_keep_real_voice_not_fixed_replies(self):
        dispatch = (MAIN / "fls2_official_board.c").read_text(encoding="utf-8")
        for command in ("AI DEMO", "AI VOICE DEMO"):
            self.assertNotIn(command, dispatch)
        for command in ("AI STATUS", "AI CANCEL"):
            self.assertIn('"' + command + '"', dispatch)
        self.assertIn("meo_voice_link_command", dispatch)
        link = (MAIN / "meoave_voice_link.c").read_text(encoding="utf-8")
        self.assertIn('strncmp(line, "VOICE ", 6)', link)
        self.assertIn('strcmp(verb, "OPEN")', link)
        for frame in ("VOICE_FRAME_META", "VOICE_FRAME_PCM", "VOICE_FRAME_END"):
            self.assertIn(frame, link)

    def test_ai_status_uses_actual_player_state(self):
        app = (MAIN / "meoave_app.c").read_text(encoding="utf-8")
        begin = app.index("static void report_ai_status(void)")
        end = app.index("\n}", begin)
        status = app[begin:end]
        self.assertIn("meo_voice_view(&voice)", status)
        self.assertIn('voice.busy ? "SPEAKING" : "IDLE"', status)
        self.assertNotIn("prompt=", status)
        self.assertNotIn("response=", status)
        self.assertNotIn("meo_conversation_", app)

    def test_production_player_has_no_embedded_voice_fixture(self):
        voice = (MAIN / "meoave_voice.c").read_text(encoding="utf-8")
        header = (MAIN / "meoave_voice.h").read_text(encoding="utf-8")
        for retired in ("meo_voice_demo", "voice_fixture", "fixture_pcm"):
            self.assertNotIn(retired, voice + header)
        for api in ("meo_voice_start", "meo_voice_cancel",
                    "meo_voice_stream_begin_sentence", "meo_voice_stream_push_pcm",
                    "meo_voice_stream_end"):
            self.assertIn(api, voice)
            self.assertIn(api, header)


if __name__ == "__main__":
    unittest.main()
