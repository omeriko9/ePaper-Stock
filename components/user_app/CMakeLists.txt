# --- User App Optimized Component Build ---

set(USERAPP_PRIV_REQUIRES
  board_power_bsp
  driver
  spiffs
  i2c_bsp
  SensorLib)

set(USERAPP_REQUIRES
  epaper_driver_bsp
  freetype
  esp_http_server
  esp_http_client
  esp_adc
  esp_wifi
  json
  nvs_flash
  audio_bsp
  button_bsp
  codec_board
  main
  espressif__arduino-esp32
  chmorgan__esp-libhelix-mp3)

idf_component_register(
  SRCS "es8311.cpp" "fetch_task.cpp" "data_cache.cpp" "ePaper.cpp" "user_app.cpp" "utilities.cpp" "adc_bsp.cpp"
       "settings.cpp" "web_server.cpp" "audio_player.cpp" "log_capture.cpp" "power_mgmt.cpp"
       "${CMAKE_CURRENT_LIST_DIR}/../i2c_equipment/i2c_equipment.cpp"
       "${CMAKE_CURRENT_LIST_DIR}/../button_bsp/button_bsp.c" "${CMAKE_CURRENT_LIST_DIR}/../button_bsp/multi_button.c"
       "${CMAKE_CURRENT_LIST_DIR}/../audio_bsp/audio_bsp.c"
  PRIV_REQUIRES ${USERAPP_PRIV_REQUIRES}
  REQUIRES ${USERAPP_REQUIRES}
  INCLUDE_DIRS "." "${CMAKE_CURRENT_LIST_DIR}/../i2c_equipment"
  # Latin subset of NotoSans (about 19 KB). Read in place from flash, so a wake
  # does not need to load the full 629 KB font from SPIFFS.
  EMBED_FILES "fonts/NotoSans-Subset.ttf")

# --- Enforce aggressive size optimization ---
add_compile_options(-Os -flto -ffunction-sections -fdata-sections)
add_link_options(-Wl,--gc-sections -flto)
