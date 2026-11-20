# Home Control

The local server hosts this webpage, polls the ESP32, and saves device state
changes in `logs.json` on this computer. The ESP32 keeps only its most recent
64 events in RAM so the server can collect them after a short interruption;
events still in RAM are lost if the ESP32 restarts. Relay states are stored
separately on the ESP32.

## Run

1. Build and flash the updated firmware. Make sure the computer and ESP32 are on
   the same network and the firmware's ESP32 address matches `192.168.1.19`.
2. From this folder, run `python server.py`.
3. Open `http://127.0.0.1:8000` in a browser. Keep the server running for
   logging and analysis.

The ESP32 address can be changed with the `ESP32_URL` environment variable.
The API key can be changed with `HOME_CONTROL_API_KEY`; use the same value as
`API_KEY` in the firmware and webpage. Only open this local server on a trusted
computer.

To check the JSON log directly, open `http://127.0.0.1:8000/logs.json`.
