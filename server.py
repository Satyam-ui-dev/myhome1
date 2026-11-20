import json
import logging
import os
import threading
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.error import URLError
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parent
LOG_PATH = ROOT / "logs.json"
ESP32_URL = os.environ.get("ESP32_URL", "http://192.168.1.19").rstrip("/")
API_KEY = os.environ.get("HOME_CONTROL_API_KEY", "change-me-123")
POLL_INTERVAL_SECONDS = 1
DEVICES = {"light", "light2", "fan", "socket"}


class LogStore:
    def __init__(self, path):
        self.path = Path(path)
        self.lock = threading.Lock()
        if self.path.exists():
            with self.path.open("r", encoding="utf-8") as file:
                data = json.load(file)
            if not isinstance(data, dict) or not isinstance(data.get("events"), list):
                raise ValueError(f"{self.path} must contain an object with an events array")
        else:
            data = {"version": 1, "events": []}
            self._write(data)

        self.data = data
        self.seen = {
            (event["boot"], event["id"])
            for event in data["events"]
            if isinstance(event, dict) and "boot" in event and "id" in event
        }
        self.status = None
        self.error = "Waiting for ESP32"

    def _write(self, data):
        temporary_path = self.path.with_suffix(self.path.suffix + ".tmp")
        with temporary_path.open("w", encoding="utf-8", newline="\n") as file:
            json.dump(data, file, indent=2)
            file.write("\n")
        os.replace(temporary_path, self.path)

    def accept_status(self, status, received_at=None):
        if not isinstance(status, dict):
            raise ValueError("ESP32 status must be a JSON object")
        boot = status.get("boot")
        uptime = status.get("up")
        raw_events = status.get("events")
        if (not isinstance(boot, int) or isinstance(boot, bool)
                or not isinstance(uptime, int) or isinstance(uptime, bool)
                or not isinstance(raw_events, list)):
            raise ValueError("ESP32 status is missing boot, up, or events")

        timestamp_now = time.time() if received_at is None else received_at
        additions = []
        for event in raw_events:
            if not isinstance(event, dict):
                continue
            event_id = event.get("id")
            event_time = event.get("t")
            device = event.get("d")
            state = event.get("s")
            if (not isinstance(event_id, int) or isinstance(event_id, bool)
                    or not isinstance(event_time, int) or isinstance(event_time, bool)
                    or not isinstance(device, str) or device not in DEVICES
                    or state not in (0, 1)):
                continue
            key = (boot, event_id)
            if key in self.seen:
                continue
            additions.append({
                "boot": boot,
                "id": event_id,
                "timestamp": max(0, int(timestamp_now - uptime + event_time)),
                "device": device,
                "state": state,
            })

        with self.lock:
            if additions:
                updated = {"version": 1, "events": self.data["events"] + additions}
                self._write(updated)
                self.data = updated
                self.seen.update((event["boot"], event["id"]) for event in additions)
            self.status = {
                key: status[key]
                for key in ("light", "light2", "fan", "socket", "up", "boot")
                if key in status
            }
            self.error = ""

    def set_error(self, message):
        with self.lock:
            self.error = message

    def status_snapshot(self):
        with self.lock:
            return {
                "connected": self.status is not None and not self.error,
                "error": self.error,
                **(self.status or {}),
            }

    def logs_snapshot(self):
        with self.lock:
            return self.data


STORE = LogStore(LOG_PATH)


def poll_esp32():
    while True:
        try:
            request = Request(
                ESP32_URL + "/api/status",
                headers={"X-API-Key": API_KEY},
            )
            with urlopen(request, timeout=4) as response:
                status = json.loads(response.read().decode("utf-8"))
            STORE.accept_status(status)
        except (URLError, TimeoutError, OSError, ValueError, UnicodeDecodeError,
                json.JSONDecodeError) as error:
            STORE.set_error(str(error))
            logging.warning("ESP32 poll failed: %s", error)
        time.sleep(POLL_INTERVAL_SECONDS)


class RequestHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(ROOT), **kwargs)

    def do_GET(self):
        if self.path == "/api/status":
            self._send_json(STORE.status_snapshot())
        elif self.path == "/api/logs":
            self._send_json(STORE.logs_snapshot())
        else:
            super().do_GET()

    def _send_json(self, payload):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)


def main():
    threading.Thread(target=poll_esp32, daemon=True).start()
    server = ThreadingHTTPServer(("127.0.0.1", 8000), RequestHandler)
    logging.info("Home Control page: http://127.0.0.1:8000")
    logging.info("Polling ESP32 at %s", ESP32_URL)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        logging.info("Stopping Home Control server")
    finally:
        server.server_close()


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    main()
