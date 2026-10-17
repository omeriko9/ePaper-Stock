# Trimmed copy of SensorLib 0.3.1: this project uses only the PCF85063 RTC driver.
idf_component_register(SRC_DIRS ./src/platform
                       INCLUDE_DIRS ./src ./src/REG ./src/platform
                       REQUIRES esp_timer esp_driver_gpio driver)
