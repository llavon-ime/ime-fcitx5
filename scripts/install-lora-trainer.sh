#!/usr/bin/env bash
# Downloads the LoRA Trainer release pinned by the lora-trainer submodule and
# installs its executable and release stamp into a target directory.
#
# The manager CLI performs the pinned-commit, manifest, SHA-256, and trainer
# API checks; this wrapper only stages the download and fixes the file modes so
# the result can be installed system-wide.
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <llavon-ime-lora> <target-dir>" >&2
    exit 2
fi
CLI="$1"
TARGET_DIR="$2"
if [[ ! -x "${CLI}" ]]; then
    echo "LoRA manager executable not found: ${CLI}" >&2
    exit 2
fi

STAGING="$(mktemp -d "${TMPDIR:-/tmp}/llavon-lora-trainer.XXXXXX")"
trap 'rm -rf "${STAGING}"' EXIT
"${CLI}" install-trainer --output-dir "${STAGING}"
# The release is a directory: the executable plus the native libraries it
# loads at startup. Replace the previous installation with the whole tree.
mkdir -p "${TARGET_DIR}"
find "${TARGET_DIR}" -mindepth 1 -maxdepth 1 -exec rm -rf {} +
cp -a "${STAGING}/." "${TARGET_DIR}/"
chmod -R a+rX "${TARGET_DIR}"
chmod 0644 "${TARGET_DIR}/trainer-release.json"
echo "Installed LoRA Trainer: ${TARGET_DIR}/llavon-lora"
