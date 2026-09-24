#!/usr/bin/env bash
# Verifies that the LoRA Trainer release pinned by the lora-trainer submodule is
# published for the requested platform. It mirrors the Windows release gate: it
# waits for the rolling manifest to report the pinned commit, then checks the
# immutable versioned manifest and the platform asset metadata. The archive
# itself is not downloaded.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PLATFORM="${1:-}"
case "${PLATFORM}" in
    linux-x64-cpu | osx-arm64-cpu) ;;
    *)
        echo "usage: $0 linux-x64-cpu|osx-arm64-cpu" >&2
        exit 2
        ;;
esac

for command in curl git python3; do
    if ! command -v "${command}" >/dev/null 2>&1; then
        echo "Required command not found: ${command}" >&2
        exit 2
    fi
done

PINNED="$(git -C "${ROOT_DIR}" ls-tree HEAD lora-trainer | awk '{print $3}')"
if [[ ! "${PINNED}" =~ ^[0-9a-f]{40}$ ]]; then
    echo "lora-trainer is not a pinned Git submodule." >&2
    exit 2
fi

ATTEMPTS="${LLAVON_IME_LORA_RELEASE_ATTEMPTS:-30}"
INTERVAL="${LLAVON_IME_LORA_RELEASE_INTERVAL:-30}"

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/llavon-lora-release.XXXXXX")"
trap 'rm -rf "${WORK_DIR}"' EXIT
ROLLING="${WORK_DIR}/rolling.json"
PINNED_MANIFEST="${WORK_DIR}/pinned.json"

MANIFEST=""
for attempt in $(seq 1 "${ATTEMPTS}"); do
    if curl --fail --silent --show-error --location --retry 3 \
        --header 'Cache-Control: no-cache' \
        --output "${ROLLING}" \
        "https://github.com/llavon-ime/lora-trainer/releases/download/latest/latest.json?commit=${PINNED}&attempt=${attempt}"; then
        commit="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("commit", ""))' "${ROLLING}")"
        if [[ "${commit}" == "${PINNED}" ]]; then
            MANIFEST="${ROLLING}"
            break
        fi
        echo "Waiting for the LoRA Trainer release of ${PINNED}; latest is ${commit}."
    else
        echo "Waiting for the first LoRA Trainer release..."
    fi
    if [[ "${attempt}" -lt "${ATTEMPTS}" ]]; then
        sleep "${INTERVAL}"
    fi
done
if [[ -z "${MANIFEST}" ]]; then
    echo "No LoRA Trainer release appeared for pinned commit ${PINNED}." >&2
    exit 1
fi

VERSION="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("version", ""))' "${ROLLING}")"
curl --fail --silent --show-error --location --retry 3 \
    --output "${PINNED_MANIFEST}" \
    "https://github.com/llavon-ime/lora-trainer/releases/download/v${VERSION}/latest.json"

export PLATFORM PINNED ROLLING PINNED_MANIFEST
python3 - <<'PY'
import json, os, re, sys

platform = os.environ["PLATFORM"]
pinned = os.environ["PINNED"]
rolling = json.load(open(os.environ["ROLLING"]))
manifest = json.load(open(os.environ["PINNED_MANIFEST"]))

def fail(message):
    sys.exit(f"LoRA Trainer release check failed: {message}")

if rolling.get("schema") != 1 or rolling.get("trainerApi") != 1 or rolling.get("commit") != pinned:
    fail("the rolling manifest does not match pinned commit schema/API/commit")
version = rolling.get("version", "")
if not re.fullmatch(r"[0-9]{4}\.[0-9]{2}\.[0-9]{2}\.[0-9]+", version):
    fail(f"invalid release version {version!r}")
if manifest.get("schema") != 1 or manifest.get("trainerApi") != 1:
    fail("the immutable manifest has an unsupported schema or API")
if manifest.get("commit") != pinned or manifest.get("version") != version:
    fail("the immutable manifest differs from the rolling manifest")
asset = (manifest.get("assets") or {}).get(platform)
name = f"llavon-lora-{version}-{platform}.tar.gz"
if not isinstance(asset, dict):
    fail(f"the immutable manifest has no {platform} asset")
if asset.get("name") != name:
    fail(f"unexpected asset name {asset.get('name')!r}")
expected_url = f"https://github.com/llavon-ime/lora-trainer/releases/download/v{version}/{name}"
if asset.get("url") != expected_url:
    fail("unexpected asset URL")
if not re.fullmatch(r"[0-9a-f]{64}", str(asset.get("sha256", ""))):
    fail("invalid asset SHA-256")
if not isinstance(asset.get("size"), int) or asset["size"] <= 0:
    fail("invalid asset size")
print(f"LoRA Trainer {version} ({pinned}) is available for {platform}.")
PY
