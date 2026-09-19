# macOS native frontend

The native macOS input method is a Swift InputMethodKit app that drives the
host-agnostic engine through its C ABI
(`engine/include/llavon_ime/llavon_ime.h`). It replaces the fcitx5-macos
frontend on macOS; Linux keeps using the fcitx5 addon.

## Layout

- `App/` — the input method app (Swift): IMK server, input controller, engine
  bridge, candidate panel, key mapping, `Info.plist`.
- `Smoke/` — a headless Swift smoke test for the C ABI (no InputMethodKit).
- `scripts/build-native-app.sh` — builds the engine, compiles the app bundle,
  signs it ad-hoc, and optionally installs it.

## Build and install

```sh
macos/scripts/build-native-app.sh            # build dist/macos/LlavonIME.app
macos/scripts/build-native-app.sh --install  # also install for this user
```

The build needs `pkg-config` (for vcpkg) and CMake; the script bootstraps
vcpkg by itself.

The install step copies the app to `~/Library/Input Methods/` and nudges
`TextInputMenuAgent`. Then enable 「拉風輸入法」 under
System Settings › Keyboard › Input Sources. If it does not show up, log out
and back in once. macOS only registers input methods whose bundle identifier
contains `.inputmethod.` (the default bundle ID is
`com.llavon.inputmethod.LlavonIME`).

The app finds its resources in this order:

1. `LLAVON_IME_*` environment overrides (`TABLE_PATH`, `TABLES_DIR`,
   `MODEL_PATH`, `UNIX_SERVICE_PATH`, `PHRASE_OVERRIDES_PATH`).
2. The package payload: `/Library/Application Support/llavon-ime/payload`
   (service + tables) and `/Library/Application Support/llavon-ime/models`.
3. The development install: `~/Library/fcitx5` (from `scripts/build-macos.sh`).

Without a service or model the engine still works with table candidates;
predictions are additive.

## Manual test checklist (TextEdit)

- Bopomofo typing, candidate list, paging, selection keys, commit.
- Smart English and the phrase marking tooltip (Shift+←, Enter, Esc).
- Switching apps/focus (composition commits on deactivate).
- Password fields (context must not be read or sent).
- Surrounding text reaching predictions.

## Smoke test (engine only)

```sh
cd fcitx5
cmake --preset macos
cmake --build --preset macos --parallel
ctest --test-dir ../build/macos --output-on-failure   # 4/4

cd ..
swiftc -O -parse-as-library \
  -I engine/include \
  macos/Smoke/Smoke.swift \
  build/macos/engine/libllavon_ime_engine.a \
  -lc++ -framework Foundation \
  -o /tmp/llavon-ime-smoke

LLAVON_IME_TABLE_PATH=ime-unix-service/ime-core/table/bopomofo_char.json \
  /tmp/llavon-ime-smoke
```

The smoke test uses `auto_start_service = 0`, so it never launches the model
service; predictions fail fast and the engine falls back to table candidates.

## C ABI contract

- All engine calls happen on the host's main thread.
- `lv_host.post` is called from background threads (prediction responses) and
  must marshal `body(body_user)` back to the main thread. `lv_engine_destroy`
  drains those callbacks; calling `body` after it returns is a no-op.
- `lv_host.commit`, `lv_host.update_ui` and `lv_host.surrounding_text` run on
  the main thread.
- `lv_engine_render` stores a snapshot that the text accessors read; the
  `update_ui` callback already refreshes it for its context.
- Offsets in `lv_surrounding_text` are UTF-16 code units.
- Key fields mirror the engine's `InputKey`: `sym` is an X11 keysym, `states`
  holds modifier bits, `release` marks key-up events (received, not consumed).

## Notes / known gaps

- Packaging still ships the fcitx5-macos setup; migrating
  `scripts/package-macos.sh`, the Homebrew tap and the release workflow to the
  native app happens once the app is verified on macOS.
- The app is unsigned (ad-hoc); distributing it needs a Developer ID and
  notarization.
- The candidate window is anchored with
  `attributes(forCharacterIndex:lineHeightRectangle:)`; coordinate adjustments
  may be needed per client app.
