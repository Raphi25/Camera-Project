"""Regression: both camera profiles must survive paths containing spaces."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "components/esp_ipa/tools/config/esp_ipa_config.py"
OV = ROOT / "main/ov5647_custom.json"
SC = ROOT / "components/esp_cam_sensor/sensors/sc2336/cfg/sc2336_default_p4_eco4.json"


class Profiles(unittest.TestCase):
    def generate(self, inputs):
        with tempfile.TemporaryDirectory(prefix="blur profiles ") as directory:
            output = pathlib.Path(directory) / "generated profiles.c"
            subprocess.run([sys.executable, str(SCRIPT), "-i", *inputs,
                            "-o", str(output), "-v", "1"], check=True)
            return output.read_text()

    def test_both_profiles(self):
        text = self.generate([str(OV), str(SC)])
        self.assertIn('"OV5647"', text)
        self.assertIn('"SC2336"', text)

    def test_single_profile(self):
        text = self.generate([str(OV)])
        self.assertIn('"OV5647"', text)
        self.assertNotIn('"SC2336"', text)

    def test_legacy_delimiter(self):
        text = self.generate([f"{OV};{SC}"])
        self.assertIn('"OV5647"', text)
        self.assertIn('"SC2336"', text)


if __name__ == "__main__":
    unittest.main()
