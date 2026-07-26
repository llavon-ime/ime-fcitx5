#!/bin/sh
set -eu

console_user="$(stat -f %Su /dev/console 2>/dev/null || true)"
if [ -z "${console_user}" ] || [ "${console_user}" = "root" ] || [ "${console_user}" = "loginwindow" ]; then
    console_user="${SUDO_USER:-}"
fi

if [ -z "${console_user}" ] || ! id "${console_user}" >/dev/null 2>&1; then
    echo "No active user found." >&2
    exit 1
fi

user_home="$(dscl . -read "/Users/${console_user}" NFSHomeDirectory 2>/dev/null | awk '{print $2}')"
if [ -z "${user_home}" ] || [ ! -d "${user_home}" ]; then
    user_home="/Users/${console_user}"
fi
if [ ! -d "${user_home}" ]; then
    echo "Home directory for ${console_user} was not found." >&2
    exit 1
fi

uid="$(id -u "${console_user}")"
if [ -L "${user_home}" ] || [ ! -d "${user_home}/Library" ] || [ -L "${user_home}/Library" ]; then
    echo "Refusing unsafe user home or Library path." >&2
    exit 1
fi
fcitx_was_running=false
if /usr/bin/pgrep -x -u "${uid}" Fcitx5 >/dev/null 2>&1; then
    fcitx_was_running=true
fi

target_root="${user_home}/Library/fcitx5"
if [ -L "${target_root}" ] || { [ -e "${target_root}" ] && [ ! -d "${target_root}" ]; }; then
    echo "Refusing unsafe target path: ${target_root}" >&2
    exit 1
fi
if [ -d "${target_root}" ] && [ -n "$(/usr/bin/find "${target_root}" -type l -print -quit 2>/dev/null)" ]; then
    echo "Refusing target tree containing symlinks: ${target_root}" >&2
    exit 1
fi

pkill -x -u "${uid}" llavon-ime-service >/dev/null 2>&1 || true
pkill -x -u "${uid}" Fcitx5 >/dev/null 2>&1 || true

rm -f \
    "${target_root}/bin/llavon-ime-service" \
    "${target_root}/lib/fcitx5/llavon-ime-addon.so" \
    "${target_root}/share/fcitx5/addon/llavon-ime.conf" \
    "${target_root}/share/fcitx5/inputmethod/llavon-ime.conf" \
    "${target_root}/plugin/llavon-ime.json"

rm -rf \
    "${target_root}/share/llavon-ime/tables/bopomofo_char.json" \
    "${target_root}/share/llavon-ime/tables/tokens"

find "${target_root}" -name '._*' -delete 2>/dev/null || true

rm -rf "/Library/Application Support/llavon-ime"
pkgutil --forget llavon-ime >/dev/null 2>&1 || true

if [ "${fcitx_was_running}" = true ] && [ -d "/Library/Input Methods/Fcitx5.app" ]; then
    /bin/launchctl asuser "${uid}" /usr/bin/open -gj -b org.fcitx.inputmethod.Fcitx5 >/dev/null 2>&1 || true
fi
