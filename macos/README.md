# macOS native frontend

The native macOS input method is a Swift InputMethodKit app that drives the
host-agnostic engine through its C ABI
(`engine/include/llavon_ime/llavon_ime.h`). It replaces the fcitx5-macos
frontend on macOS; Linux keeps using the fcitx5 addon.

## Layout

- `App/` — the input method app (Swift): IMK server, input controller, engine
  bridge, candidate panel, key mapping, `Info.plist`.
- `Tests/` — `CoreTests.swift`, pure-function tests of the platform-independent
  Swift core (`scripts/verify-core.sh`). Input behaviour is not tested here:
  it belongs to the shared raw-key suite (see Tests below).
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
home directory instead, for machines without sudo.

Moving the bundle makes the text input system drop the input source, which is
what leaves the input menu without 「拉風輸入法」 after an install. The step
therefore re-registers and re-enables the input source with the same helper the
package builds (`packaging/macos/tools/tis.c`), and puts it back in the input
menu when it was in use before (or when it is still registered but was dropped
from the menu). The package postinstall does the same and launches the app once
after the payload is installed, so an upgrade keeps working without a log out.
Then enable 「拉風輸入法」 under
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

One standard behaviour suite, shared by every frontend, plus unit tests for the
parts keys cannot reach:

- `engine/tests/rawkey/` (`llavon_ime_rawkey_tests`) — the standard suite: raw
  keys in, panel state and commits out, driven through the host-free engine API
  that both frontends use. It runs on Linux and macOS, and scenarios can point
  the transport at a scripted or real prediction service. All input behaviour
  (bopomofo, smart English, candidate navigation, symbol menu, phrase
  overrides, keypad, lifecycle, prediction) lives here.
- `engine/tests/` (`llavon_ime_tests`) — engine unit tests for internals that
  cannot be expressed as keys: protocol framing, config parsing, UTF handling,
  the service transport and the C ABI contract.
- `macos/scripts/verify-core.sh` — pure-function Swift core: key translation,
  the config schema/values model and the candidate paging math.

```sh
scripts/verify-engine-tests.sh   # unit + raw-key suites (Linux and macOS)
macos/scripts/verify-core.sh     # Swift core
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

LLAVON_IME_TABLE_PATH=ime-core/table/bopomofo_char.json \
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
