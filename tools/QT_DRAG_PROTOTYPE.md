# Bluepaws PySide6 drag-performance prototype

This prototype answers one question before the TLV desktop console is migrated:
does a representative PySide6/Qt interface drag smoothly on the target Windows
computer?

It deliberately creates both full tabs at startup, including scrolling forms,
dozens of fields and checkboxes, a table, packet output, JSON preview and response
log. This is a meaningful rendering workload rather than an empty-window test.
It does not load credentials, calculate production packets or send network
requests.

## Install and run on Windows

From the repository root, use a dedicated environment so Qt cannot conflict with
the existing CustomTkinter console:

```powershell
py -3.11 -m venv .venv-qt
& ".\.venv-qt\Scripts\python.exe" -m pip install --upgrade pip
& ".\.venv-qt\Scripts\python.exe" -m pip install -r ".\tools\requirements-qt-prototype.txt"
& ".\.venv-qt\Scripts\python.exe" ".\tools\tlv_simulator_qt_prototype.py" --check
& ".\.venv-qt\Scripts\python.exe" ".\tools\tlv_simulator_qt_prototype.py"
```

Expected dependency check:

```text
Bluepaws Qt prototype dependencies available (PySide6 6.11.1).
```

Drag the populated window repeatedly around the screen, then switch tabs and
repeat the test. The complete contents should remain visible and attached to the
title bar throughout; there is no lightweight placeholder workaround.

If this test is smooth, the next change will retain `tlv_packet_codec.py` and
port only the user-interface and network orchestration from
`tlv_simulator_gui.py` to PySide6.
