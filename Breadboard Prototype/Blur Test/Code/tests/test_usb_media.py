"""Unit tests for TinyUSB volume discovery and verified copying."""

import sys
import tempfile
import threading
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gui"))
import usb_media


class FakePort:
    def __init__(self, response):
        self.response = bytearray(response)
        self.commands = []

    @property
    def in_waiting(self):
        return len(self.response)

    def read(self, size):
        data = bytes(self.response[:size])
        del self.response[:size]
        return data

    def write(self, data):
        self.commands.append(data)


class UsbMediaTests(unittest.TestCase):
    def test_request_ignores_boot_log_before_response(self):
        port = FakePort(b"booting\nMSC_READY BLUR_001122AABBCC\n")
        result = usb_media.request(
            port, "USB_MSC_START", "MSC_READY ", threading.Event(), timeout=1
        )
        self.assertEqual(result, "MSC_READY BLUR_001122AABBCC")
        self.assertEqual(port.commands, [b"USB_MSC_START\n"])

    def test_inventory_accepts_only_capture_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            volume = Path(temporary)
            (volume / "BLUR_DEVICE.TXT").write_text("BLUR_001122AABBCC\n", "ascii")
            run = volume / "blur" / "run_00001"
            run.mkdir(parents=True)
            capture = run / "000001_v050_a30.jpg"
            sidecar = run / "000001_v050_a30.jpg.csv"
            capture.write_bytes(b"jpeg")
            sidecar.write_text("metadata", "ascii")
            (run / "notes.txt").write_text("ignore", "ascii")
            self.assertEqual(
                usb_media.inventory(volume, "BLUR_001122AABBCC"),
                [capture, sidecar],
            )

    def test_copy_verified_is_atomic_and_reports_hash(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source.jpg"
            destination = root / "output" / "image.jpg"
            source.write_bytes(b"camera-data" * 100)
            result = usb_media.copy_verified(source, destination, threading.Event())
            self.assertEqual(destination.read_bytes(), source.read_bytes())
            self.assertEqual(result["bytes"], source.stat().st_size)
            self.assertFalse(destination.with_name("image.jpg.part").exists())


if __name__ == "__main__":
    unittest.main()
