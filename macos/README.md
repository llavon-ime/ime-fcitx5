# macOS native frontend

The native macOS input method is a Swift InputMethodKit app that drives the
host-agnostic engine through its C ABI
(`engine/include/llavon_ime/llavon_ime.h`). It replaces the fcitx5-macos
frontend on macOS; Linux keeps using the fcitx5 addon.

## Layout

- `App/` — the input method app (Swift): IMK server, input controller, engine
  bridge, candidate panel, key mapping, `Info.plist`.
- `Tests/` — Swift test programs: `CoreTests.swift` (platform-independent core,
  `scripts/verify-core.sh`) and `InputHarness.swift` (raw NSEvents through the
  input controller, `scripts/verify-input-harness.sh`).
- `Smoke/` — a headless Swift smoke test for the C ABI (no InputMethodKit).
- `scripts/build-native-app.sh` — builds the engine, compiles the app bundle,
  signs it ad-hoc, and optionally installs it.

## Build and install

```sh
macos/scripts/build-native-app.sh            # build dist/macos/LlavonIME.app
macos/scripts/build-native-app.sh --install  # install the app and the service
```

The build needs `pkg-config` (for vcpkg) and CMake; the script bootstraps
vcpkg by itself.

`--install` also builds, tests and installs the AI prediction service through
`scripts/build-macos-service.sh`: `/Library/Application Support/llavon-ime/payload`
for a system install (the path the package uses and the app prefers),
`~/Library/fcitx5` with `--user`. The model under
`/Library/Application Support/llavon-ime/models` is reused when present and
downloaded otherwise. `--no-service` skips the service and only installs the
app.

The install step copies the app to `/Library/Input Methods/`, the same place
the package uses, so a package install and a development install never shadow
each other. It removes a `~/Library/Input Methods/` copy first, because a
second bundle with the same identifier shadows the system one (and makes the
package installer relocate its bundle). `--install --user` installs into the
home directory instead, for machines without sudo. The step then nudges
`TextInputMenuAgent`. Then enable 「拉風輸入法」 under
System Settings › Keyboard › Input Sources. If it does not show up, log out
and back in once. macOS only registers input methods whose bundle identifier
contains `.inputmethod.` (the default bundle ID is
`com.llavon.inputmethod.LlavonIME`).

## Caps Lock switching

To switch between 「拉風輸入法」 and English with Caps Lock, enable
「使用大寫鎖定鍵切換至／從…」 (Use the Caps Lock key to switch to and from)
under System Settings › Keyboard › Input Sources. The input source must stay
non-Latin for this: `tsInputMethodCharacterRepertoireKey` and
`tsInputModeCharacterRepertoireKey` must not list `Latn`. Adding `Latn` marks
the input source as ASCII-capable, so macOS classifies it as a Latin source
and the Caps Lock switch no longer treats it as the Chinese side.

## Settings

The input menu lists 「設定…」 under 「拉風輸入法」. Settings are saved to
`~/.config/llavon-ime/config.json` (XDG aware) and phrase overrides live in
`~/.config/llavon-ime/phrase_overrides.txt`, shared with the fcitx5 addon;
「編輯替代詞彙…」 in the settings window opens the file in the default editor.
Until the settings are saved for the first time, an existing
`~/Library/Application Support/fcitx5/conf/llavon-ime.conf` from the
fcitx5-based frontend keeps working.

The settings window is rendered from the engine's config schema
(`engine/src/config/config_schema.cpp`), which also drives the fcitx5 addon
configuration: adding an option to that schema is enough, because JSON/INI
(de)serialization, the schema export in the C ABI and both config UIs derive
from it. No Swift or addon code has to change for a new option.

The model path follows the same rule as the fcitx5 addon: the value saved in
the settings file, otherwise the installed model under
`/Library/Application Support/llavon-ime/models`. `LLAVON_IME_MODEL_PATH` only
overrides the path the prediction service is started with; it is not written
back to the settings.

The app finds its resources in this order:

1. `LLAVON_IME_*` environment overrides (`TABLE_PATH`, `TABLES_DIR`,
   `MODEL_PATH`, `UNIX_SERVICE_PATH`, `PHRASE_OVERRIDES_PATH`).
2. The package payload: `/Library/Application Support/llavon-ime/payload`
   (service + tables) and `/Library/Application Support/llavon-ime/models`.
3. The development install: `~/Library/fcitx5` (from `scripts/build-macos-service.sh`).

Without a service or model the engine still works with table candidates;
predictions are additive.

## Manual test checklist (TextEdit)

- Bopomofo typing, candidate list, paging, selection keys, commit.
- Smart English and the phrase marking tooltip (Shift+←, Enter, Esc).
- Switching apps/focus (composition commits on deactivate).
- Password fields (context must not be read or sent).
- Surrounding text reaching predictions.

## Tests

Three layers, mirroring the fcitx5 side (which drives the addon through
fcitx5's TestFrontend):

- `macos/scripts/verify-input-harness.sh` — raw input at the frontend: built
  `NSEvent`s go through the real `LlavonInputController` (key translation,
  engine bridge, marked text and commits on a recording `TextClient`), and the
  harness asserts both keysym shapes AppKit may report for punctuation.
  `IMKInputController` rejects clients that are not real IMK proxies, so the
  controller is built through its `init(testClient:)` seam; the IMK session
  itself stays untested.
- `macos/scripts/verify-core.sh` — platform-independent core: key translation,
  the config schema/values model and the candidate paging math.
- `engine/tests/` (ctest) — the engine and the C ABI contract, including the
  shape sweep that keeps the two frontends' key shapes identical.

```sh
macos/scripts/verify-input-harness.sh   # raw NSEvent -> controller -> client
macos/scripts/verify-core.sh            # Swift core
ctest --test-dir build/engine-tests --output-on-failure
```

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
