#!/usr/bin/env bash
set -euo pipefail

BACKEND_DIR="${1:-}"
if [[ ! -d "${BACKEND_DIR}" ]]; then
    echo "Usage: $0 /path/to/ggml/backend/directory" >&2
    exit 2
fi

for command in file grep nm objdump; do
    if ! command -v "${command}" >/dev/null 2>&1; then
        echo "Missing required command: ${command}" >&2
        exit 2
    fi
done

shopt -s nullglob
backends=("${BACKEND_DIR}"/libggml-cpu-*.so)
shopt -u nullglob
if [[ ${#backends[@]} -lt 2 ]]; then
    echo "Expected multiple ggml CPU backends under ${BACKEND_DIR}, found ${#backends[@]}." >&2
    exit 1
fi

temporary_dir="$(mktemp -d)"
trap 'rm -rf "${temporary_dir}"' EXIT
with_avx512=0
without_avx512=0

for backend in "${backends[@]}"; do
    if ! file -b "${backend}" | grep -q '^ELF '; then
        continue
    fi

    symbols="$(nm -D --defined-only "${backend}")"
    if ! grep -Eq '[[:space:]]ggml_backend_init$' <<< "${symbols}" || \
       ! grep -Eq '[[:space:]]ggml_backend_score$' <<< "${symbols}"; then
        echo "Missing dynamic backend exports in ${backend}." >&2
        exit 1
    fi

    disassembly="${temporary_dir}/$(basename "${backend}").txt"
    objdump -d --no-show-raw-insn "${backend}" > "${disassembly}"
    if grep -Eq '\bzmm[0-9]+\b' "${disassembly}"; then
        ((with_avx512 += 1))
    else
        ((without_avx512 += 1))
    fi
done

if [[ ${with_avx512} -eq 0 || ${without_avx512} -eq 0 ]]; then
    echo "CPU dispatch validation failed: AVX512=${with_avx512}, non-AVX512=${without_avx512}." >&2
    exit 1
fi

echo "CPU dispatch backends verified: AVX512=${with_avx512}, non-AVX512=${without_avx512}."
