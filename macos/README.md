# macOS native frontend

This directory hosts the native macOS frontend. The engine is host-agnostic and
exposes a C ABI (`engine/include/llavon_ime/llavon_ime.h`) so a Swift
InputMethodKit input method can drive it directly.

## Current status

- `Smoke/` is a headless Swift smoke test for the C ABI (keys, commits,
  renders, callbacks). It does not use InputMethodKit yet.
- The InputMethodKit app itself still has to be written; see "Next steps".

## Build the smoke test (macOS)

Build and install the engine first, using the existing macOS preset:

```sh
cd fcitx5
cmake --preset macos
cmake --build --preset macos --parallel
```

Then compile and run the smoke test against the engine archive:

```sh
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

## Next steps (InputMethodKit app)

1. `IMKInputController` subclass registered as a system input method
   (`.inputmethod` bundle with `tsInputMethodCharacterRepertoireKey` etc.),
   with `recognizedEvents` including `NSEventMaskKeyUp`.
2. Forward every key event through `lv_engine_key_event`; consume the event
   when it returns 1.
3. Render `lv_render_info` as marked text (`setMarkedText:selectionRange:`)
   plus a candidate window; `lv_engine_preedit_segment` carries the underline
   flags and `lv_engine_aux_up` the phrase-marking tooltip.
4. Implement `lv_host.surrounding_text` from the client's
   `NSTextInputClient` state and `is_sensitive` from secure-input state.
5. Bundle the service binary, tables and model, or reuse the installed
   `/Library/Application Support/llavon-ime` copies, passing their paths in
   `lv_engine_options`.
