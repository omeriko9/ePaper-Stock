idf_component_register(
                       SRCS "WiFiManager.cpp"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES arduino-esp32 nvs_flash
)

# Turn off format warnings for this 3rd-party lib to prevent -Werror interference
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format)
