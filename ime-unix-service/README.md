# IME Unix Service

Unix socket service for Llavon IME on Linux and macOS. It provides
session-based inference to the Linux Fcitx5 addon and native macOS app.
Model loading, tokenization, and llama.cpp inference are provided by the
`ime-core` submodule at the repository root.

## Build

From the repository root, initialize its submodules and pass the vcpkg toolchain
from the build environment or CI:

```bash
git submodule update --init --recursive
cd ime-unix-service
cmake --preset linux \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DIME_UNIX_SERVICE_BUILD_TESTS=ON
cmake --build --preset linux
ctest --test-dir build/linux --output-on-failure
cmake --install build/linux
```

The install output includes the service and the canonical tables from
`ime-core` (under the configured install prefix):

```text
bin/llavon-ime-unix-service
bin/llavon-ime-lora
bin/llavon-ime-lora-gui
share/llavon-ime/tables/
```

## Run

Pass the required model file and tables directory explicitly:

```bash
dist/bin/llavon-ime-unix-service \
  --model path/to/llavon-ime-llama-250m-Q4_K_M.gguf \
  --tables path/to/tables
```

The service also accepts configuration through the `LLAVON_IME_MODEL_PATH`,
`LLAVON_IME_TABLES_DIR`, and `LLAVON_IME_UNIX_SOCKET_PATH` environment
variables. Frontends consume the installed executable rather than adding the
service as a CMake subdirectory.

## Optional local LoRA training

The IME's **收集個人化訓練資料** option is off by default. When enabled,
completed, non-sensitive Bopomofo commits are sent to the per-user service and
stored in `${XDG_STATE_HOME:-$HOME/.local/state}/llavon-ime/training/commits.sqlite3`
on Linux or `~/Library/Application Support/llavon-ime/training/commits.sqlite3`
on macOS.
`LLAVON_IME_TRAINING_DATABASE_PATH` overrides that file. Text is stored locally
and is never sent to the model download service. The Unix socket and the
database are restricted to the current user. Use `llavon-ime-lora list` to
review pending entries and `llavon-ime-lora exclude --id ID` or `delete --id ID`
to remove entries before training.
Deleting a previously trained entry removes its saved text and readings; it
does not undo an adapter that has already been trained.

The input method menu's **管理個人化訓練…** action starts
`llavon-ime-lora-gui`, an independent local browser interface. On macOS the
settings window has a **使用我的輸入改進模型** button; on Linux the same button is in
the Fcitx5 input method settings. As on Windows, the page selects pending
records (all selected by default), checks or downloads the fixed checkpoint,
offers the same training defaults (rank/alpha 8/16, dropout 0, batch size and
accumulation 1, 5 epochs, max steps -1, learning rate 1e-4, FP32,
`q_proj,v_proj`, device `auto`), shows progress and run history, and can cancel
its process group. The page polls the database, so records typed while it is
open appear automatically and join the selection by default; starting a run
excludes the unchecked records it is showing.
Records that cannot be converted remain pending. Explicit manual candidate
choices contribute three samples, while other records contribute one. A
completed GGUF can be selected for inference from its history row. Each record
is rendered as a ruby-annotated preview in the style of the validation web UI;
the page looks readings up in the installed character table
(`bopomofo_char.json`), falls back to the table when a stored reading is
missing, and flags a reading the table does not list for that character.
The manager invokes the separate `llavon-ime-lora` CLI. The
manager binds only to `127.0.0.1` on a random port and uses a per-launch
access token. Opening it again reuses the running manager; a manager built
from newer sources replaces an idle running instance so the browser never
stays on an outdated interface, while an instance with an active job is left
alone. After the page is closed it exits on idle, while an active training job
keeps it running.
The installed Linux package uses `xdg-open` and macOS uses `/usr/bin/open` to
open the default browser. To launch it from a terminal instead, run
`llavon-ime-lora-gui` (or its packaged private executable path).

`llavon-ime-lora` is a separate command-line manager; it does not run Torch
inside the input method or the prediction service. The GUI's **安裝／更新 LoRA
Trainer** action downloads the platform's official, SHA-256 verified CPU
release from [lora-trainer](https://github.com/llavon-ime/lora-trainer),
installs it below the user's training state directory, and uses it automatically.
The whole release directory is installed, because TorchSharp loads the native
libraries that ship next to the executable; an archive that only carries the
executable is rejected, and an installation without those libraries is never
treated as ready.
The installer is idempotent: when the installed executable still matches the
pinned commit and its recorded SHA-256, nothing is downloaded, and verified
archives are cached under `${XDG_CACHE_HOME:-$HOME/.cache}/llavon-ime/lora-trainer`
so a second target or a later packaging run reuses them
(`LLAVON_IME_LORA_TRAINER_CACHE` overrides that directory).
As on Windows, `lora-trainer` is a pinned source submodule: its Git commit
selects the matching release binary, but the app does not build Torch from the
submodule. The installer waits for `latest.json` to report that exact commit,
then checks the immutable versioned release manifest and the platform archive's
SHA-256 before publishing the executable. A different release commit is never
substituted. `scripts/build-linux.sh` performs the same verified download at
install time and places the trainer under
`<private library dir>/llavon-ime/tools/lora`; set
`LLAVON_IME_SKIP_LORA_TRAINER` to skip it. The deb, RPM, and macOS packages
bundle that pinned release during packaging (CI downloads it in the release
workflow), so an install already carries the trainer; the GUI action then
updates it in place for the current user.
For development and tests `LLAVON_IME_LORA_CLI_PATH` overrides the
executable and `LLAVON_IME_LORA_ASSETS_DIR` overrides the checkpoint and run
root, matching the Windows service variables.
The manager checks the executable's `--version --json` result for trainer
API 2 rather than trusting the release manifest's API field.
On Debian/RPM packages, the manager is at
`/usr/lib/llavon-ime/llavon-ime-lora` or
`/usr/lib64/llavon-ime/llavon-ime-lora`; on macOS it is at
`/Library/Application Support/llavon-ime/payload/bin/llavon-ime-lora`.
The examples below assume its directory has been added to `PATH`.

For example:

```sh
state="$HOME/.local/state/llavon-ime/training"
assets="$state/assets"
llavon-ime-lora fetch-model --output-dir "$assets"
llavon-ime-lora install-trainer --output-dir "$state/tools/lora"
revision="$(cat "$assets/current.revision")"
model_dir="$assets/$revision"

llavon-ime-lora list
llavon-ime-lora dataset --model-dir "$model_dir" \
  --tables-dir /path/to/installed/share/llavon-ime/tables \
  --output "$assets/review.jsonl"
llavon-ime-lora train --model-dir "$model_dir" \
  --tables-dir /path/to/installed/share/llavon-ime/tables \
  --revision "$revision" \
  --output-dir "$state/runs/first"
```

The manager checks the checkpoint revision, model vocabulary, and compatible
table layout, then validates numeric JSONL with the trainer before starting.
The current inference tables add two token IDs beyond the public training
checkpoint's vocabulary; the manager projects the known additions back to
the checkpoint-compatible table and leaves unsupported commits pending.
Training defaults to auto/float32; `--device cuda` requires a CUDA-capable
trainer. Each run writes an adapter and a Q4_K_M GGUF model. Like Windows,
subsequent runs resume from the last adapter when rank, alpha, dropout, and
target modules match, and reuse that adapter's recorded base checkpoint
revision instead of the directory just passed. Training history records the
parent run, cumulative record count, optimizer steps, and LoRA parameters.
Entries are marked trained only after the model has been exported; the
original inference model remains active until the resulting GGUF is selected
in the GUI's history. Linux reloads Fcitx5 settings and recreates the inference
transport for the new model; macOS receives a local notification, re-reads
the saved config and restarts the prediction service.
