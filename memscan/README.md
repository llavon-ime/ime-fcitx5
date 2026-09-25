# llavon-ime-memscan

A deliberately small helper that finds a probe token in the memory of the
user's own processes and returns the text in front of it. It is the last
context source of the input method for applications that expose neither client
surrounding text nor AT-SPI (for example Chromium, CEF, xterm, Tk, Telegram).

The helper is intentionally *not* a generic memory reader:

* the token must be either a private-use-area string (>= 12 codepoints) or a
  `LVP`-prefixed ASCII debugging token (>= 16 alphanumeric characters);
* only PIDs owned by the calling user are accepted;
* only a bounded window (at most 64 KiB each side, default 4 KiB before) in
  front of the token is returned;
* scanning is bounded in bytes and wall-clock time.

## Build and test

```sh
cmake -S memscan -B build/memscan -DCMAKE_BUILD_TYPE=Release
cmake --build build/memscan --parallel
ctest --test-dir build/memscan --output-on-failure
```

## Usage

```sh
llavon-ime-memscan --needle "<probe text>" --pid 1234 [--pid 5678]
                   [--before 4096] [--after 512]
                   [--expect-suffix "<just committed text>"]
                   [--hint 1234:0x55550000-0x55560000]
                   [--timeout-ms 3000] [--max-bytes 536870912]
                   [--all-mappings] [--same-uid-all]
```

`--expect-suffix` is the text the input method just committed. The caret sits
right after it, so the window in front of the token ends with that text for the
document copy; matches are preferred accordingly, which keeps protocol buffers
and layout caches (which also contain the token) from being chosen.

`--same-uid-all` scans every process owned by the caller instead of an
explicit PID list (clients such as XIM do not tell the input method which
process they are). The caller itself and its parent - the input method that
spawned the helper - are skipped, so the input method's own copy of the probe
token never matches. Candidates are ordered by resident set size.

Output is one JSON object on stdout:

```json
{"found":true,"pid":1234,"encoding":"utf16le","address":"0x5555a1b2c3d4",
 "before":"hello magic ","after":"seed line\n",
 "before_bytes":12,"after_bytes":11,"scanned_bytes":4194304,"elapsed_ms":7}
```

Exit codes: `0` found, `1` not found, `2` usage/needle error, `3` permission
denied or foreign PID, `4` timeout/byte budget. A permission failure is
reported as `{"found":false,"error":"denied"}`, which the engine surfaces as
"memory context unavailable" instead of pretending the token was absent.

`--hint <pid>:<start>-<end>` points the scanner at a region that contained the
token during the previous probe (the engine caches it), so repeat probes stay
fast.

Setting `MEMSCAN_DEBUG=1` prints the raw window bytes of a hit to stderr; it is
meant for development only.

## Permission

Reading another process needs the same UID plus ptrace permission. Yama
(`kernel.yama.ptrace_scope=1`) refuses non-descendant access, so the helper
needs either `CAP_SYS_PTRACE`

```sh
sudo setcap cap_sys_ptrace+ep /usr/libexec/llavon-ime/llavon-ime-memscan
```

or `kernel.yama.ptrace_scope=0`. Descendant processes (the test suite) need
neither.
