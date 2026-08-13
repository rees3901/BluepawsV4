# Bluepaws PySide6 TLV desktop console

`tlv_simulator_qt.py` is the primary desktop interface for manually building and
sending Bluepaws v1.1 telemetry. PySide6 replaced CustomTkinter after a populated
230-widget Qt prototype demonstrated normal Windows window movement on the target
development PC. The former `tlv_simulator_gui.py` remains temporarily available
as a fallback, but new desktop work should target this Qt console.

The interface delegates packet construction, authentication, wrapper validation
and HTTP posting to `tlv_packet_codec.py`, which is also used by the VPS simulator.

## Install and run on Windows

From the repository root, use a dedicated environment so Qt cannot conflict with
the existing CustomTkinter console:

```powershell
py -3.11 -m venv .venv-qt
& ".\.venv-qt\Scripts\python.exe" -m pip install --upgrade pip
& ".\.venv-qt\Scripts\python.exe" -m pip install -r ".\tools\requirements-qt-gui.txt"
& ".\.venv-qt\Scripts\python.exe" ".\tools\tlv_simulator_qt.py" --check
& ".\.venv-qt\Scripts\python.exe" ".\tools\tlv_simulator_qt.py"
```

Expected dependency check:

```text
Bluepaws Qt console dependencies available (PySide6 6.11.1).
```

## Safe use

Load the private device credentials file on the packet-builder tab. Secrets stay
masked and are excluded from wrapper previews and response logs. For LTE direct,
use the selected device bearer token. For a LoRa home-hub relay, replace it with
the provisioned gateway bearer token and provide the gateway GUID16.

The console supports valid packets and deliberate negative HMAC tests, custom
TLVs, wrapper preview, repeated or advancing sequences, cancellable background
HTTP posting and timestamped response logging. Use test devices only. Never put
a Supabase service-role key in the credential file or bearer field.

Expected responses include `201` for a new observation, `200` with
`duplicate: true` for an idempotent retry, `400` for malformed input, `401` for
invalid credentials/HMAC, `409` for an identity conflict and `503` for an
ingestion/database-stage failure.
