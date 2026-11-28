# MCLA PC Launcher (standalone)

A transparent WebView2 launcher UI for **Midnight Club: Los Angeles (PC recomp)**.

![Launcher demo](launcher-demo.gif)

The visual target is the game's own menus: the same typography, spacing, motion and HUD language, so the launcher feels like part of Midnight Club rather than a generic AI-looking settings wrapper.

This is a **separate project** from the game (`mcla pc`). It produces `mcla_launcher.exe`,
which you copy next to `mcla.exe` in the game's `build/` directory. It never modifies
`mcla.exe`.

## Why it exists

The old in-tree launcher was a GDI+ dialog. This one is a 650×446 CSS-layout, borderless, **transparent**
WebView2 window hosting the launcher UI from `ui/index.html`. The host is PerMonitorV2-aware and
scales its physical bounds to the foreground monitor's DPI while keeping WebView2 zoom at 1.0.
True per-pixel alpha is achieved with Win32 `WS_EX_NOREDIRECTIONBITMAP` + DirectComposition and
`ICoreWebView2Controller3::put_DefaultBackgroundColor({0,0,0,0})`.

Measured dead ends (do not re-try):

| approach | result |
|---|---|
| WinForms `TransparencyKey = magenta` | magenta renders **visible** (WebView2 child HWND ignores parent color key) |
| WPF `AllowsTransparency=True` | transparent, but WebView2 **input is dead** |
| C++ Win32 + DirectComposition (here) | **transparent + input works** — verified |

## Build

Requirements: VS BuildTools (vcvars64), LLVM `clang-cl`, WebView2 Runtime installed.

```bat
build.bat
```

Output: `out\mcla_launcher.exe` — a **single executable**. The WebView2 loader, the UI, its fonts and
the icon are embedded as resources and extracted to `%LOCALAPPDATA%\MCLA_Launcher\WebView2\` on
first run. There is no `launcher\` folder next to the game.

The installer accepts a single `.iso` file (CHOOSE ISO). It first downloads `mcla.exe` from the
game's GitHub release (`engine-v1`) and verifies its SHA-256, then validates the ISO9660 header,
extracts the measured game-file map from `src/iso_manifest.h`, writes a clean `cache/mcla.toml`,
removes development leftovers (`*.log`, `*.pdb`, `*.dmp`), and warms the PSO/shader caches by
running `mcla.exe` once with `MCLA_PSO_PREWARM=1`. Building the game from source on the player's
machine is not supported: HEAD cannot build without ~780 MB of untracked `.research` dependencies
plus MSVC/Windows SDK. The game directory ends up with only the launcher,
`mcla.exe`, `game_data\`, `cache\`, `dxil_cache\` and `mods\`.

The host auto-detects paths:
- `mcla.exe` — next to the exe, or `..\build\mcla.exe`
- UI — `<exe dir>\launcher\ui`, or `<repo>\tools\launcher\ui`
- `cache/mcla.toml` — `<exe dir>\cache`, or `<repo>\build\cache`

## First-run installer

The launcher does not start the game until installation is complete. On startup it checks
`cache/mcla_ready.json`. The marker is created **only** after every required item is present
and verified; deleting or changing a required file invalidates it.

Required items:

- `mcla.exe` (SHA-256 recorded in the marker)
- `cache/mcla.toml` (SHA-256 recorded in the marker)
- `launcher\ui\index.html` (SHA-256 recorded in the marker)
- `game_data\` (file count + total bytes recorded in the marker)

If the marker is missing or invalid, the **PLAY** tab itself shows the setup rows — welcome,
language, source folder, CHOOSE FOLDER, CHOOSE ISO, INSTALL and CANCEL — using the same rows,
header, footer, highlight and navigation as the game menu. There is no separate SETUP tab.
The player selects one of six languages (English, العربية, Français, Español, Deutsch, Italiano);
Arabic flips the whole window to RTL. Every other tab and every settings row is shown at half
opacity and cannot be changed until installation completes. Game settings come from one source
only: `cache/mcla.toml`, created during installation. The ready marker is written only after
verification succeeds, and it signs game files only (not the launcher UI), so updating the
launcher never re-locks the game. `launchGame` refuses while the marker is invalid.

Rejected inputs are mapped to actions in `docs/BAD_INPUT_MAP.md` and logged to
`launcher_errors.log`. After installation the launcher also offers manual backup/restore of
`cache/mcla.toml.bak`, a GitHub release check, gamepad navigation (LB/RB tabs, A accept, B back,
left stick up/down), and double-press confirmation for reset, restore and quitting with unsaved
changes.

After installation the launcher exposes: DISPLAY additions (brightness, gamma, aspect ratio,
HDR10, reported resolution, HUD scale), GRAPHICS presets (LOW/MEDIUM/HIGH), CONTROLS presets
(WASD/XBOX/CUSTOM) with double-press reset confirmation, ADVANCED CVars grouped by
RENDER/AUDIO/ENGINE/DEBUG, a manual `cache/mcla.toml.bak` backup button, and LAUNCHER settings
for changing the language.

Host bridge actions added for this flow: `getInstallStatus`, `chooseInstallSource`,
`runInstall`, `setInstallLanguage`, `cancelInstall`. The host answers with `loadInstallStatus`,
`setInstallSource`, `setInstallProgress`, `installFinished`.

`out\mcla_launcher.exe --selftest` includes a fresh-install simulation in a temporary folder:
missing marker → install → marker created → tampered file invalidates it. Exit code `0` means
the whole flow passed.

## Release package

Run `powershell -ExecutionPolicy Bypass -File tools\make_release.ps1` after `build.bat`.
It creates `out\mcla-launcher-<tag>.zip` containing only the launcher executable, its WebView2
loader, the icon and `launcher\ui\`. Game binaries and retail content are **not** included.

## Host commands (AGENT_HANDOFF.md §4)

| action | handler |
|---|---|
| `ready` | reads `cache/mcla.toml` and calls `MCLALauncherAPI.loadConfig(json)` |
| `dragWindow` | `WM_NCLBUTTONDOWN/HTCAPTION` (also top 24px strip via `WM_NCHITTEST`) |
| `launchGame` | merges CVars into `cache/mcla.toml`, writes startup keys to `mcla_launcher.ini [env]`, spawns `mcla.exe` |
| `saveConfig` | merges CVars into `cache/mcla.toml` and writes startup keys to `[env]` |
| `browseFiles` / `openLogs` | opens game dir / cache dir |
| `minimizeWindow` / `exitLauncher` | window control |

## Verify

1. `Start-Process build\mcla_launcher.exe`
2. `PrintWindow` capture → UI renders; desktop `ImageGrab` crop → desktop shows through.
3. Click a nav row → UI navigates (input works).
4. Keyboard: Up/Down select, Left/Right change, Enter accepts, Tab moves, Q/E switch tabs, Escape returns.
5. Keymap rows accept every key supported by the game; click again or outside to cancel capture.
6. Driving uses one binding per control: `W` throttle, `S` brake, `A/D` steer. Duplicate raw `LT/RT` axis rows are hidden so one trigger cannot be bound twice.
6. `build/mcla.exe` mtime unchanged.

### Regression notes (2026-11-24)

- Screen entry animations can outlive the 95 ms screen swap. Their delayed highlight is now cancelled when leaving a screen, entry delays are capped, and the highlight is applied after the final row has entered. Without this, fast Q/E or tab-button navigation could leave the new screen with no selected row and appear stuck.
- The game's CVar config reader is flat. Launcher schema sections such as `mcla`, `renderer`, and `game` are UI grouping labels, not TOML tables. The host writes their keys at the top of `cache/mcla.toml`.
- Startup-only settings are mirrored to `mcla_launcher.ini [env]`: render scale, display mode, VSync, FPS cap, LOD, AO and shadows. This prevents an old launcher section or flat TOML value from silently overriding the current UI choice.
- `MCLA_FLOW_LOD` is allowlisted, and `MCLA_FPS_CAP` uses the game's accepted literals (`30`, `60`, `120`, `uncapped`, or `0` for no override).
- `out\mcla_launcher.exe --selftest` exercises repeated writes, quote/unicode escaping, the env allowlist, FPS, AO and shadows. Exit code must be `0`.
