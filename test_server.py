import json
import tempfile
import unittest
from pathlib import Path

from server import LogStore


class LogStoreTests(unittest.TestCase):
    def setUp(self):
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.log_path = Path(self.temporary_directory.name) / "logs.json"
        self.store = LogStore(self.log_path)

    def tearDown(self):
        self.temporary_directory.cleanup()

    def test_saves_events_and_ignores_repeated_poll_data(self):
        status = {
            "boot": 123,
            "up": 100,
            "events": [{"id": 1, "t": 40, "d": "light", "s": 1}],
        }

        self.store.accept_status(status, received_at=1000)
        self.store.accept_status(status, received_at=1001)

        saved = json.loads(self.log_path.read_text(encoding="utf-8"))
        self.assertEqual(len(saved["events"]), 1)
        self.assertEqual(saved["events"][0], {
            "boot": 123,
            "id": 1,
            "timestamp": 940,
            "device": "light",
            "state": 1,
        })

    def test_event_id_is_unique_per_boot(self):
        self.store.accept_status({
            "boot": 123,
            "up": 10,
            "events": [{"id": 1, "t": 2, "d": "fan", "s": 1}],
        }, received_at=100)
        self.store.accept_status({
            "boot": 456,
            "up": 10,
            "events": [{"id": 1, "t": 3, "d": "fan", "s": 0}],
        }, received_at=200)

        self.assertEqual(len(self.store.logs_snapshot()["events"]), 2)

    def test_persisted_event_is_not_duplicated_after_server_restart(self):
        status = {
            "boot": 123,
            "up": 10,
            "events": [{"id": 1, "t": 2, "d": "fan", "s": 1}],
        }
        self.store.accept_status(status, received_at=100)
        restarted_store = LogStore(self.log_path)
        restarted_store.accept_status(status, received_at=101)

        self.assertEqual(len(restarted_store.logs_snapshot()["events"]), 1)


if __name__ == "__main__":
    unittest.main()
