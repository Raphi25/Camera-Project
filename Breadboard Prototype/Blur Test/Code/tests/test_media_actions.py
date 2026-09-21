"""Tests for the GUI worker without creating a Tk window."""

import queue
import sys
import threading
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gui"))
import GUI


class FakePort:
    def __enter__(self):
        return self

    def __exit__(self, *_):
        return None


class MediaActionTests(unittest.TestCase):
    @staticmethod
    def make_app():
        app = GUI.App.__new__(GUI.App)
        app.events = queue.Queue()
        app.stop = threading.Event()
        return app

    def test_worker_runs_transfer_and_returns_to_idle(self):
        app = self.make_app()
        with patch.object(GUI.serial, "Serial", return_value=FakePort()), \
                patch.object(GUI, "transfer") as transfer:
            app.run("COM5", False)
        transfer.assert_called_once()
        self.assertEqual(list(app.events.queue)[-1], ("idle", None))

    def test_worker_reports_transfer_error(self):
        app = self.make_app()
        with patch.object(GUI.serial, "Serial", return_value=FakePort()), \
                patch.object(GUI, "transfer", side_effect=RuntimeError("media failed")):
            app.run("COM5", False)
        self.assertIn(("status", "media failed"), list(app.events.queue))
        self.assertEqual(list(app.events.queue)[-1], ("idle", None))


if __name__ == "__main__":
    unittest.main()
