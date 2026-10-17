# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(epaper_stock)


# --- Global Build Optimization Settings ---
add_compile_options(-Os -flto -ffunction-sections -fdata-sections)
add_link_options(-Wl,--gc-sections -flto)
set(CMAKE_BUILD_TYPE Release)

spiffs_create_partition_image(storage spiffs FLASH_IN_PROJECT)
spiffs_create_partition_image(audio audio FLASH_IN_PROJECT)
