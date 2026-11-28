# MCLA launcher — fidelity plan to the in-game pause menu

Status: **plan only, no implementation yet**. Owner approved a detailed plan before code.

## 1. Reference material already extracted

Source video: `Midnight Club Los Angeles 60fps - RPCS3 Configuration settings Benchmark.mp4`.

| Timestamp | Frame | Use |
|---|---|---|
| 1.6 s | `PAUSE` | simple two-row menu, header icons, footer |
| 2.2 s | `MODES` | full six-row menu, L1/R1 boxes, icon set |

Working crops are in `%TEMP%\opencode\rpc_full\` and `%TEMP%\opencode\rpc_sharp\`.
`docs/reference_pause_menu.png` is the committed crop from the earlier pass. The plan is to
replace/extend it with sharpened, full-resolution crops of both frames.

## 2. Findings from frame-by-frame analysis

### 2.1 English font

The original HUD font is **Ooh Bahn Fixed Width** by Benjamin Posch (credited by the MCLA HUD
port community and the game UI). It is a fixed-width, technical DIN-like face.

- Current launcher tries `local('Ooh Bahn')` first, then Bahnschrift, then Saira/Rajdhani.
- Most machines do not have Ooh Bahn installed, so the fallback is what users see.
- Licence: **free for personal use only**; commercial use needs the author's licence.

Decision needed from owner before bundling the actual font file (see open question Q1).

### 2.2 Panel frame

The in-game panel is two visual layers:

1. **Outer frame** — crisp silver/gray outline, rounded but nearly straight corners, inner shadow.
2. **Inner panel** — dark translucent fill with a subtle blur of the 3D scene behind it.

Current launcher has a single border with soft translucency, which reads as one blurry layer.

### 2.3 Selected tab ring

The frame-diff analysis (21 consecutive 60 fps frames around the active tab) shows:

- the **dashed ring is static**;
- a **bright arc/comet travels around it**;
- the arc changes position while the dash pattern stays fixed.

Current implementation rotates the whole dashed ring. This must change to a static ring plus a
moving glow overlay. Direction must mirror with RTL.

### 2.4 Header buttons and icons

The game header contains, left→right:

- `L1` grey shoulder box;
- pause icon inside a dashed ring;
- gamepad icon inside a dashed ring + yellow tab title;
- checkmark icon inside a dashed ring;
- `R1` grey shoulder box.

Current launcher has `Q`/`E` prompt SVGs and simple dashed icons, but not the full set.

### 2.5 Rows and navigation

- Row text is white; selected row text is cyan on a teal gradient with a small triangle.
- Row alignment stays per-language (right for RTL, left for LTR) per owner decision.
- Navigation: tabs only with Q/E or shoulder buttons; arrows change the highlighted row's value.
- Footer: `EXIT / ESC` and `ACCEPT / ENTER` mirrored in RTL.

## 3. Implementation phases

### Phase A — reference assets

1. Extract clean 1280×720 frames at 1.6 s and 2.2 s with ffmpeg.
2. Apply `lanczos` upscale + `unsharp`, save to `docs/reference_pause_*.png`.
3. Add a small comparison note listing measured colors (yellow `#e8de2a`, cyan `#44f6f6`,
   panel fill, border gray).

Acceptance: both reference images are committed and viewable.

### Phase B — English font

1. If owner approves bundling: obtain `OohBahn-Regular.otf`/WOFF2, convert to WOFF2, add to
   `ui/fonts/ooh-bahn-fixed-width.woff2`, add `@font-face` with `font-display: swap`.
2. Put `Ooh Bahn Fixed Width` first in the font stack; keep `MCLA_AR` first via `unicode-range`
   for Arabic so RTL text is unaffected.
3. If owner does **not** approve bundling: keep `local('Ooh Bahn')` and improve the fallback
   stack only; document that exact fidelity requires the user-installed font.
4. Register the new font in `tools/gen_rc.py` and `ensureBundledResources()` so it is embedded
   in the single executable.

Acceptance: English text in the launcher matches the reference at the same size; no Arabic
regression.

### Phase C — two-layer panel

1. In `ui/index.html`, wrap the existing `.hud-screen` in an outer frame element or add
   `::before`/`::after` layers.
2. Outer layer: crisp `1–2px` border, rgba gray, small radius, inset shadow.
3. Inner layer: existing dark translucent fill + `backdrop-filter: blur(...)` matching the game.
4. Ensure both layers participate in RTL and the 3D tilt animation.

Acceptance: screenshot comparison shows a distinct outer frame and blurred inner panel.

### Phase D — header icons and buttons

1. Extend `makeDashedIconSvg()` with `pause`, `gamepad`, `check` symbols.
2. Extend `makePromptIcon()` to render the game-style `L1`/`R1` shoulder boxes (currently Q/E
   for PC input; keep Q/E as the keyboard equivalent in the title attribute).
3. Set the active tab icon to the gamepad symbol used in the reference.
4. Keep all icons inside dashed rings; keep the yellow title.

Acceptance: header matches the reference icon order and styling in both LTR and RTL.

### Phase E — moving glow, static ring

1. Keep the dashed circle static: remove `animation` from `.dash-ring`.
2. Add a second SVG element (arc) or a CSS `conic-gradient` mask over the same ring:
   - arc length ~25–35% of circumference;
   - color yellow-green/white with soft falloff;
   - rotation `6s linear infinite` (owner to confirm speed/color).
3. Under `html[dir="rtl"]`, set `animation-direction: reverse` for the glow.
4. Do not rotate the inner icon.

Acceptance: 60 fps frame capture of the launcher shows a stationary dash pattern and a moving
highlight arc, mirrored in Arabic.

### Phase F — rows, footer, full RTL sweep

1. Keep row alignment per language (already decided).
2. Re-check separators, font sizes, cyan selected text, triangle, footer prompts.
3. Apply frame/glow changes to every tab, not just the first.
4. Re-run the full interaction matrix: every row, cycle, text input, key binding, action button,
   locked state, six languages, new font, both directions.

### Phase G — delivery

1. `node --check` on the inline script.
2. `build.bat`, `--selftest`, fresh-directory install test.
3. Commit named paths, push, upload the single-file release asset, verify SHA-256.

## 4. Open questions for the owner

- **Q1 — Ooh Bahn licence:** bundle the personal-use-only font in a public repo, or rely on
  `local('Ooh Bahn')` and document it?
- **Q2 — Glow colour/speed:** exact colour and rotation period; owner said "see the frame", so
  default is sampled from the video unless overridden.
- **Q3 — Shoulder labels:** show `L1/R1` like the game, or keep `Q/E` with the game-style box?
- **Q4 — Reference set:** sharpen both frames or keep the committed crop as legacy?

## 5. Risks

- Bundling Ooh Bahn may violate its personal-use-only licence in a public repository.
- Heavy backdrop blur costs GPU time in WebView2; keep blur radius small and measure.
- Arabic `Noto Sans Arabic` and English `Ooh Bahn` must not affect each other; enforce with
  `unicode-range` and test both directions.
