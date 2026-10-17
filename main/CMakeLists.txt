idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "./"
                    REQUIRES freetype log wifimanager user_app nvs_flash i2c_equipment i2c_bsp board_power_bsp SensorLib)
