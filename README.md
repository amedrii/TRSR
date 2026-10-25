# TRSR — Tomb Raider Speedrunning

TRSR contains the website, matchmaking backend, and Tomb Raider race clients. TRASR is the Tomb Raider: Anniversary desktop client.

TRASR is a small Windows desktop app for racing Tomb Raider: Anniversary head-to-head. It handles Speedrun.com sign-in, matchmaking, opponent status, race forfeits, synchronized run starts, and automatic finish submission. Built with C++17, Qt 6, a Cloudflare Worker backend, and screen-based completion detection.

## Start

For the current prototype, download the Windows build from [TRASR releases](https://github.com/amedrii/TRASR/releases), extract it, and run `TRASR.exe`. Keep all files distributed alongside the executable together. Portable releases bundle their Qt, screen-verifier, and Tesseract runtimes: players do not need to install Python, Tesseract, or Python packages.

1. Start TRASR and sign in with Speedrun.com.
2. Select the level, category, and subcategory.
3. Join either the random queue or selected-ruleset queue.
4. Wait until a match is found.
5. Open Tomb Raider: Anniversary and leave it on the main menu with **Play Game** highlighted.
6. Both players press **Ready** in TRASR.
7. During the five-second countdown, focus the game window.
8. TRASR navigates to the selected level and starts both players together.
9. Finish the level normally. TRASR detects the completion screen and submits the result.

Use **Forfeit match** if you want to leave an active match. This immediately awards the match to your opponent.

## Behavior

- Uses Speedrun.com authentication for player identity.
- Matches players by level, category, and subcategory.
- Displays opponent state: waiting, ready, navigating menus, armed, running, restarted, finished, or forfeited.
- Coordinates a shared race start after both players are ready.
- Does not send menu input unless Tomb Raider: Anniversary is focused.
- Watches for the game's completion screen before submitting a finish.
- Awards the match to the first accepted valid finish.
- Records match results and forfeits in the hosted backend.
- Does not use time-trial completion as a requirement.

## Current limitations

- Automatic menu navigation currently supports **Final Conflict** only.
- Before both players press Ready, each player must manually leave the game on the main menu with **Play Game** selected.
- Screen-based completion detection is still being tested across levels and display setups.
- TRASR is currently a prototype and is not yet intended for ranked racing.

## Build

Requirements: Windows, Qt 6 with Widgets and Network, CMake, a matching C++17 compiler, Python 3.13+, and Tesseract OCR.

Install the detector's Python dependencies:

    py -3.13 -m pip install -r tools\requirements-level-end-detector.txt

For source builds, install Tesseract OCR and ensure it is available at:

    C:\Program Files\Tesseract-OCR\tesseract.exe

Open `CMakeLists.txt` in Qt Creator, select an installed Qt Desktop kit, configure a Release build, and build the `TRASR` target.

To produce the portable ZIP after building the Release target, run:

    powershell -ExecutionPolicy Bypass -File tools\build-portable-release.ps1 -Version 0.2.1

This release-build step freezes the helper scripts, bundles Tesseract, deploys the Qt runtime, and creates an upload-ready ZIP. Python and Tesseract are required only on the PC that creates the release.

## Backend deployment

The `worker` directory contains the Cloudflare Worker and D1 database migrations.

Apply new SQL files in `worker/migrations` to the production D1 database in numeric order before deploying the updated Worker. The backend stores matchmaking state, player presence, scheduled starts, match outcomes, and forfeits.

## Website

The new static website is in `web/`; see [website notes](web/README.md). The root `index.html` remains the current placeholder page. Deploy the updated Worker and verify production data before switching GitHub Pages to the new site.

## Validation

The completion detector requires the selected level's title and the level-completion screen to remain visible for multiple checks before it emits a completion event.

Race starts are cancelled safely if the game window is not focused, the menu navigation cannot finish, or the selected level does not load.

Live matchmaking, completion detection, and level-end routes are still actively being verified. Reports for additional level-end paths and unusual game states are especially useful.

## Source layout

- `main.cpp`: application entry point.
- `trasr_window.*`: user interface, matchmaking, race controls, and opponent status.
- `run_monitor.*`: game monitoring, integrity checks, and finish submission.
- `screen_completion.*`: reads completion events from the screen detector.
- `tra_process.*`: Tomb Raider: Anniversary process and memory reader.
- `tools/`: completion detection and menu-navigation scripts.
- `worker/`: Cloudflare Worker source and D1 migrations.
- `web/`: static website source.

## Disclaimer

TRASR is an unofficial fan-made project. It is not affiliated with Crystal Dynamics, Eidos, Square Enix, Tomb Raider, or Speedrun.com.
