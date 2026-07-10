file(READ "${ADDON_CONF}" addon_conf)
file(READ "${INPUTMETHOD_CONF}" inputmethod_conf)

if(NOT addon_conf MATCHES "(^|\n)OnDemand=True(\n|$)")
    message(FATAL_ERROR "fcitx5 addon must use OnDemand=True when inputmethod metadata provides the IM")
endif()

if(NOT inputmethod_conf MATCHES "(^|\n)Addon=llavon-ime(\n|$)")
    message(FATAL_ERROR "fcitx5 inputmethod metadata must reference addon llavon-ime")
endif()

if(NOT inputmethod_conf MATCHES "(^|\n)Name=拉風輸入法(\n|$)")
    message(FATAL_ERROR "fcitx5 inputmethod display name must be 拉風輸入法")
endif()

if(NOT inputmethod_conf MATCHES "(^|\n)Label=拉風(\n|$)")
    message(FATAL_ERROR "fcitx5 inputmethod label must be 拉風")
endif()
