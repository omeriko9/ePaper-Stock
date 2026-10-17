#ifndef BUTTON_BSP_H
#define BUTTON_BSP_H
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#ifdef __cplusplus
extern "C" {
#endif

extern EventGroupHandle_t boot_groups;
extern EventGroupHandle_t pwr_groups;
#define set_bit_button(x) ((uint32_t)(0x01)<<(x))
#define get_bit_button(x,y) (((uint32_t)(x)>>(y)) & 0x01)
#define set_bit_all 0x00ffffff                   //最高24bit


//set bit
#define set_bit_data(x,y) (x |= (0x01<<y))
#define clr_bit_data(x,y) (x &= ~(0x01<<y))
#define get_bit_data(x,y) ((x>>y) & 0x01)
#define rset_bit_data(x) ((uint32_t)0x01<<(x))

void user_button_init(void);
uint8_t user_button_get_repeat_count(void);
uint8_t user_boot_get_repeat_count(void);
bool user_buttons_are_idle(void);  // Returns true if both buttons are in IDLE state

// Unified button events and queue registration for background handler
#include "freertos/queue.h"

typedef enum {
    BTN_EVT_BOOT_SINGLE = 0,
    BTN_EVT_BOOT_LONG   = 1,
    BTN_EVT_BOOT_DOUBLE = 2,
    BTN_EVT_PWR_SINGLE  = 3,
    BTN_EVT_PWR_LONG    = 4,
    BTN_EVT_PWR_DOUBLE  = 5
} button_event_t;

// Register a queue to receive button_event_t messages from button callbacks
void button_register_event_queue(QueueHandle_t queue);


#ifdef __cplusplus
}
#endif
#endif
