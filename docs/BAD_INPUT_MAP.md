# Bad input → launcher action map

Every rejected input is routed through `mclaBadInput(kind, detail)`. The launcher shows the
matching toast and appends a timestamped line to `launcher_errors.log`.

| Kind | Trigger | Action |
|---|---|---|
| `locked-row` | Click/arrow/text change on any row or tab while the ready marker is invalid | Ignore the change, show "INSTALL THE GAME FIRST" |
| `bad-iso` | Chosen file is not an ISO9660 image (`CD001` at sector 16) | Reject file, keep previous ISO, show "SELECT A VALID .ISO FILE" |
| `missing-source` | INSTALL pressed with no source folder | Show "CHOOSE A SOURCE FOLDER" |
| `bad-path` | Source path contains invalid characters or is empty | Reject, do not copy |
| `invalid-language` | Host receives an unknown language code | Fall back to `en`, show map message |
| `hash-mismatch` | Game file changed after install | Invalidate marker, relock settings, prompt reinstall |
| `install-failed` | Copy or verification failed | Keep marker absent, show error, log detail |
| `backup-failed` | `cache/mcla.toml.bak` missing or copy failed | Show "BACKUP FAILED", keep current config untouched |
| `reset-not-confirmed` | First press of RESET KEYMAP or RESTORE SETTINGS | Arm for 3 s, show "PRESS AGAIN TO CONFIRM" |
| `quit-dirty` | QUIT pressed while settings were changed and not saved | Arm for 3 s, show "UNSAVED SETTINGS — SAVE FIRST" |
| `unknown` | Any unmapped action | Ignore and log |

## Locked-input rules (before installation)

- Only the PLAY tab is usable, because it hosts the setup rows.
- All other tabs and every settings row get `.is-locked` (half opacity).
- Locked rows ignore mouse click, arrow keys, text input and gamepad activation.
- The host independently refuses `optionChanged`, `saveConfig`, `restoreDefaults`,
  preset changes, keymap reset and `launchGame` while the marker is invalid.
