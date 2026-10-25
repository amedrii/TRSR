# Level-end screen detector (prototype)

This helper watches only the visible client area of the `Tomb Raider: Anniversary` window. It declares a result only after the selected level name, `COMPLETION TIME`, and `Continue` have all appeared in three consecutive OCR passes. It does not read input, change the game, or trust a signal ID.

## One-time setup

Install a normal 64-bit Python for Windows, then run:

```powershell
python -m pip install -r tools\requirements-level-end-detector.txt
```

Also install the Tesseract OCR application. Its `tesseract.exe` needs to be on your PATH, or pass its complete path with `--tesseract`.

## Test command

Start the game and then run this in a second terminal. It waits until the Mountain Caves result screen is visible, writes one event to the chosen file, and exits.

```powershell
python tools\level_end_detector.py --level "Mountain Caves" --event-file "$env:LOCALAPPDATA\TRASR\level-end-events.jsonl" --once
```

`completion_time_ocr` in the event is diagnostic only. TRSR must keep using the read-only in-game timer for the authoritative time. The `--webhook` option is intentionally not connected yet: the live Worker has no authenticated match-results endpoint, and the desktop client should submit the confirmed in-game time once that endpoint exists.