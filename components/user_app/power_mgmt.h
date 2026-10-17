#pragma once

#include <stdint.h>
#include "esp_system.h"

/*
 * Board power control for deep sleep.
 *
 * Battery power on this board goes through a latch: GPIO17 (BAT_Control)
 * drives transistor T2 through a 1k resistor, and T2 keeps P-FET Q4 on.
 * If GPIO17 drops while on battery, the whole board turns off.
 *
 * A strong output-high on GPIO17 pulls about 2.5 mA through the 1k base
 * resistor, all the time the device sleeps. The internal pull-up (~45k)
 * gives enough base current for T2 on most boards, for about 0.06 mA.
 * We use the pull-up by default and fall back to the strong drive if the
 * board ever turns off during a pull-up sleep (see power_latch_check_on_cold_boot).
 */

#ifdef __cplusplus
extern "C" {
#endif

// Take back the pads that deep sleep held, drive the latch strongly while awake,
// and turn the audio rail on (the I2C bus needs it, see power_enter_deep_sleep).
// Call first in app_main, before any other GPIO setup.
void power_init_on_boot(void);

// Detect a latch failure from the previous pull-up sleep and arm the detector
// for the next one. Needs NVS and the I2C bus (RTC chip). Call on cold boot only.
void power_latch_check_on_cold_boot(esp_reset_reason_t reset_reason);

// Put the always-powered I2C parts (SHTC3, RTC clock output) into their lowest
// power state. They keep that state until power is removed, so call on cold boot only.
void power_quiet_i2c_peripherals(void);

// Release all board pads for minimum leakage and enter deep sleep.
// sleep_us == 0 means: wake only on the BOOT button.
void power_enter_deep_sleep(uint64_t sleep_us) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
