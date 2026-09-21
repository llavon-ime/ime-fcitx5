#!/bin/sh
set -eu

# Uninstalls the native input method package and cleans up the files left by
# the earlier fcitx5-based package.

console_user="$(stat -f %Su /dev/console 2>/dev/null || true)"
if [ -z "${console_user}" ] || [ "${console_user}" = "root" ] || [ "${console_user}" = "loginwindow" ]; then
    console_user="${SUDO_USER:-}"
fi

if [ -n "${console_user}" ] && id "${console_user}" >/dev/null 2>&1; then
    uid="$(id -u "${console_user}")"
    /usr/bin/pkill -x -u "${uid}" LlavonIME >/dev/null 2>&1 || true
fi

rm -rf "/Library/Input Methods/LlavonIME.app"
rm -rf "/Library/Input Methods/Fcitx5.app"
rm -rf "/Library/Application Support/llavon-ime"
pkgutil --forget llavon-ime >/dev/null 2>&1 || true

if [ -n "${console_user}" ] && id "${console_user}" >/dev/null 2>&1; then
    user_home="$(dscl . -read "/Users/${console_user}" NFSHomeDirectory 2>/dev/null | awk '{print $2}')"
    if [ -z "${user_home}" ] || [ ! -d "${user_home}" ]; then
        user_home="$(eval echo "~${console_user}")"
    fi

    # Legacy files from the fcitx5-based package.
    target_root="${user_home}/Library/fcitx5"
    /usr/bin/pkill -x llavon-ime-unix-service >/dev/null 2>&1 || true
    /usr/bin/pkill -x Fcitx5 >/dev/null 2>&1 || true
    rm -f \
        "${target_root}/bin/llavon-ime-unix-service" \
        "${target_root}/lib/fcitx5/llavon-ime-addon.so" \
        "${target_root}/share/fcitx5/addon/llavon-ime.conf" \
        "${target_root}/share/fcitx5/inputmethod/llavon-ime.conf" \
        "${target_root}/plugin/llavon-ime.json"
    rm -rf "${target_root}/share/llavon-ime/tables"
    find "${target_root}" -name '._*' -delete 2>/dev/null || true
fi

exit 0
