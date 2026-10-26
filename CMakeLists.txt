# Phone remote (docs/phone-remote.md). Nothing is built unless enabled.
if(CONFIG_TOTEM_PHONE_REMOTE)
  zephyr_library()
  # <lvgl.h> sits at the LVGL module root, which Zephyr does not export.
  zephyr_library_include_directories(${ZEPHYR_LVGL_MODULE_DIR})
  zephyr_library_sources(
    src/phone_remote/remote.c
    src/phone_remote/remote_hid.c
    src/phone_remote/remote_proto.c
    src/phone_remote/remote_screen.c
  )
endif()
