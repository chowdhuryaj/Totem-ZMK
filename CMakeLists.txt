# Dongle-only modules; nothing is built unless enabled (the halves build
# none of them).
if(CONFIG_TOTEM_PHONE_REMOTE OR CONFIG_TOTEM_MOUSE_KEYS OR CONFIG_TOTEM_MOUSE_HOST_FLASK OR CONFIG_TOTEM_MOUSE_SCREEN)
  zephyr_library()
endif()

# Phone remote (docs/phone-remote.md).
if(CONFIG_TOTEM_PHONE_REMOTE)
  # <lvgl.h> sits at the LVGL module root, which Zephyr does not export.
  zephyr_library_include_directories(${ZEPHYR_LVGL_MODULE_DIR})
  zephyr_library_sources(
    src/phone_remote/remote.c
    src/phone_remote/remote_hid.c
    src/phone_remote/remote_proto.c
    src/phone_remote/remote_screen.c
  )
endif()

# BLE mouse host: buttons as key positions, Flask channel (docs/mouse-host.md).
zephyr_library_sources_ifdef(CONFIG_TOTEM_MOUSE_KEYS src/mouse_host/mouse_keys.c)
zephyr_library_sources_ifdef(CONFIG_TOTEM_MOUSE_HOST_FLASK src/mouse_host/mouse_flask.c)

# Mouse status on the Prospector screen (src/mouse_screen/mouse_screen.c).
if(CONFIG_TOTEM_MOUSE_SCREEN)
  zephyr_library_include_directories(${ZEPHYR_LVGL_MODULE_DIR})
  zephyr_library_sources(src/mouse_screen/mouse_screen.c)
endif()
