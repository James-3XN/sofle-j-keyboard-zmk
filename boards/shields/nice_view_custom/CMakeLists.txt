if(CONFIG_ZMK_DISPLAY AND CONFIG_NICE_VIEW_WIDGET_STATUS)
  # Firmware version shown by the version key: the commit the build was made from. GitHub
  # Actions provides it as GITHUB_SHA; local builds fall back to git, then to "local".
  if(DEFINED ENV{GITHUB_SHA})
    string(SUBSTRING "$ENV{GITHUB_SHA}" 0 7 NV_FW_VERSION)
  else()
    execute_process(
      COMMAND git -c safe.directory=* rev-parse --short=7 HEAD
      WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
      OUTPUT_VARIABLE NV_FW_VERSION
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
  endif()
  if(NOT NV_FW_VERSION)
    set(NV_FW_VERSION "local")
  endif()
  zephyr_compile_definitions(NV_FW_VERSION="${NV_FW_VERSION}")

  zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include)
  zephyr_library_sources(custom_status_screen.c)
  zephyr_library_sources(widgets/bolt.c)
  zephyr_library_sources(widgets/util.c)
  zephyr_library_sources(widgets/display_invert.c)
  zephyr_library_sources(widgets/battery_pct.c)
  zephyr_library_sources(widgets/version_toggle.c)
  if(CONFIG_ZMK_BACKLIGHT)
    zephyr_library_sources(widgets/backlight_redraw.c)
  endif()

  if(NOT CONFIG_ZMK_SPLIT OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    zephyr_library_sources(widgets/status.c)
    zephyr_library_sources(widgets/volume_sync.c)
  else()
    zephyr_library_sources(widgets/peripheral_status.c)
  endif()

  if(CONFIG_LAYER_LEDS)
    zephyr_library_sources(widgets/layer_leds.c)
  endif()
  if(CONFIG_ZMK_RGB_UNDERGLOW)
    zephyr_library_sources(widgets/underglow_dim_step.c)
  endif()
endif()
