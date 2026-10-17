#include "button_bsp.h"
#include "multi_button.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "user_config.h"
#include "freertos/timers.h"
#include "freertos/queue.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declare C++ AudioPlayer functions for use in C code
void audio_player_play_nonblocking(const char* filename);

// From user_app (C wrappers)
void user_request_refresh(void);
#ifdef __cplusplus
}
#endif

static const char* TAG = "button_bsp";

EventGroupHandle_t boot_groups;
EventGroupHandle_t pwr_groups;
static QueueHandle_t s_button_evt_queue = NULL; // optional, set by user via registration

static Button button1;                  //申请按键
#define USER_KEY_1 BOOT_BUTTON_PIN      //实际的GPIO
#define button1_id 1                    //按键的ID
#define button1_active 0                //有效电平 (active LOW - boot button shorts to GND when pressed)


static Button button2;                  //申请按键
#define USER_KEY_2 PWR_BUTTON_PIN       //实际的GPIO
#define button2_id 2                    //按键的ID
#define button2_active 0                //有效电平

// Timer handle for button state machine (only runs when button activity detected)
static TimerHandle_t button_timer = NULL;
static volatile bool button_activity = false;

/*******************回调事件声明***************/
static void on_boot_single_click(Button* btn_handle);
static void on_boot_long_press(Button* btn_handle);
static void on_boot_double_click(Button* btn_handle);
static void on_pwr_single_click(Button* btn_handle);
static void on_pwr_long_press(Button* btn_handle);
static void on_pwr_double_click(Button* btn_handle);

/*********************************************/

// FreeRTOS timer callback (replaces esp_timer periodic callback)
static void button_timer_callback(TimerHandle_t xTimer)
{
    static int idle_ticks = 0;  // Count ticks in idle state before stopping timer
    
    // Process button state machine FIRST
    button_ticks();  // State machine update
    
    // Uncomment for detailed button state debugging:
    // static int log_counter = 0;
    // static int last_state = BTN_STATE_IDLE;
    // if (button1.state != BTN_STATE_IDLE) {
    //     esp_rom_printf("BTN1: state=%d lvl=%d gpio=%d ticks=%d\n", 
    //         button1.state, button1.button_level, gpio_get_level(USER_KEY_1), button1.ticks);
    //     log_counter++;
    //     idle_ticks = 0;
    // } else if (last_state != BTN_STATE_IDLE) {
    //     esp_rom_printf("BTN1: -> IDLE (was in state for %d ticks)\n", log_counter);
    //     log_counter = 0;
    // }
    // last_state = button1.state;
    
    // CRITICAL: Keep timer running while button1 is active OR there was recent activity
    // Give the state machine several ticks after going idle to process double-click detection
    int btn1_busy = (button1.state != BTN_STATE_IDLE);
    
    if (!btn1_busy && !button_activity)
    {
        idle_ticks++;
        // Wait at least 100 ticks (500ms) after idle before stopping to allow double-click detection
        if (idle_ticks > 100)
        {
            xTimerStop(button_timer, 0);
            // esp_rom_printf("BTN timer stopped after %d idle ticks\n", idle_ticks);
            idle_ticks = 0;
        }
    }
    else
    {
        idle_ticks = 0;  // Reset if there's activity
    }
    
    // Reset activity flag AFTER checking - this gives state machine one tick to process
    button_activity = false;
}

static uint8_t read_button_GPIO(uint8_t button_id)   //返回GPIO电平
{
	switch (button_id)
  	{
  	  	case button1_id:
  	  	  	return gpio_get_level(USER_KEY_1);
  	  	case button2_id:
  	  	  	return gpio_get_level(USER_KEY_2);
  	  	default:
  	  	  	break;
  	}
  	return 1;
}

// GPIO interrupt handler - starts button timer when button pressed
static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    // gpio_num_t gpio_num = (gpio_num_t)(int)arg;
    // int level = gpio_get_level(gpio_num);
    // esp_rom_printf("BTN ISR: GPIO%d=%d\n", gpio_num, level);
    
    button_activity = true;
    
    // Start/restart the button timer from ISR
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (button_timer != NULL) {
        if (xTimerIsTimerActive(button_timer) != pdFALSE) {
            xTimerResetFromISR(button_timer, &xHigherPriorityTaskWoken);
        } else {
            xTimerStartFromISR(button_timer, &xHigherPriorityTaskWoken);
        }
    }
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

static void gpio_init(void)
{
    // Install GPIO ISR service first
    gpio_install_isr_service(0);
    
    // Configure BOOT button (GPIO 0) - ACTIVE LOW
    // Hardware: Button connects VBAT to GPIO0, but VBAT circuit latches during operation
    // During normal operation: GPIO0 has external pull-up, button press shorts to GND via circuit
    // In deep sleep: Button press pulls VBAT high to wake system
    gpio_config_t gpio_conf = {};
    gpio_conf.intr_type = GPIO_INTR_ANYEDGE;  // Trigger on both press (falling) and release (rising)
    gpio_conf.mode = GPIO_MODE_INPUT;
    gpio_conf.pin_bit_mask = (0x1ULL<<USER_KEY_1);
    gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;  // Pull-up to detect active-low button
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));
    
    // PWR button (GPIO 18) DISABLED - physically always pressed, not configured
    // gpio_conf.intr_type = GPIO_INTR_ANYEDGE;
    // gpio_conf.pin_bit_mask = (0x1ULL<<USER_KEY_2);
    // gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    // gpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    // ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));
    
    // Add ISR handler for BOOT button only
    gpio_isr_handler_add(USER_KEY_1, gpio_isr_handler, (void*) USER_KEY_1);
    // gpio_isr_handler_add(USER_KEY_2, gpio_isr_handler, (void*) USER_KEY_2);
    
    ESP_LOGI(TAG, "Button GPIO interrupt configured (GPIO %d only, GPIO %d disabled)", USER_KEY_1, USER_KEY_2);
}

void user_button_init(void)
{
  	boot_groups = xEventGroupCreate();
  	pwr_groups = xEventGroupCreate();
  	gpio_init();

  	button_init(&button1, read_button_GPIO, button1_active , button1_id);       	// 初始化 初始化对象 回调函数 触发电平 按键ID
  	button_attach(&button1,BTN_SINGLE_CLICK,on_boot_single_click);            	    //单击事件             
  	button_attach(&button1,BTN_LONG_PRESS_START,on_boot_long_press);            	    //长按事件

  	// BUTTON2 (PWR) DISABLED - physically always pressed, ignore completely
  	// button_init(&button2, read_button_GPIO, button2_active , button2_id);
  	// button_attach(&button2,BTN_SINGLE_CLICK,on_pwr_single_click);
  	// button_attach(&button2,BTN_LONG_PRESS_START,on_pwr_long_press);

    // Create FreeRTOS software timer (5ms period) - ONLY runs when button activity detected
    button_timer = xTimerCreate(
        "btn_timer",
        pdMS_TO_TICKS(5),  // 5ms period
        pdTRUE,            // Auto-reload
        NULL,
        button_timer_callback
    );
    
    if (button_timer == NULL)
    {
        ESP_LOGE(TAG, "Failed to create button timer");
        return;
    }
    
    // Timer starts in stopped state - will be started by GPIO interrupt
    ESP_LOGI(TAG, "Button timer created (on-demand mode for ALS compatibility)");
    
    // Enable double click events for BOOT button only
    button_attach(&button1,BTN_DOUBLE_CLICK,on_boot_double_click);
    // button_attach(&button2,BTN_DOUBLE_CLICK,on_pwr_double_click);
    
  	// button_start(&button2); // DISABLED - button2 ignored
  	button_start(&button1); //启动按键
}


/*事件函数*/
/*
 * CRITICAL: These callbacks run in Timer Service task context (limited stack).
 * Keep them MINIMAL - only set event bits and wake the main task.
 * DO NOT call audio_player, logging, or any heavy functions here.
 */

/*BOOT button - 单击*/
static void on_boot_single_click(Button* btn_handle)
{
    // ONLY set event bit - main task will handle everything else
    xEventGroupSetBits(boot_groups, set_bit_button(0));
    // Post unified event if queue is registered
    if (s_button_evt_queue) {
        button_event_t evt = BTN_EVT_BOOT_SINGLE;
        (void)xQueueSend(s_button_evt_queue, &evt, 0);
    }
}

/*BOOT button - 长按*/
static void on_boot_long_press(Button* btn_handle)
{
    // ONLY set event bit - main task will handle deep sleep entry
    xEventGroupSetBits(boot_groups, set_bit_button(1));
    if (s_button_evt_queue) {
        button_event_t evt = BTN_EVT_BOOT_LONG;
        (void)xQueueSend(s_button_evt_queue, &evt, 0);
    }
}

/*BOOT button - double click*/
static void on_boot_double_click(Button* btn_handle)
{
    // ONLY set event bit - main task will handle sound playback
    xEventGroupSetBits(boot_groups, set_bit_button(2));
    if (s_button_evt_queue) {
        button_event_t evt = BTN_EVT_BOOT_DOUBLE;
        (void)xQueueSend(s_button_evt_queue, &evt, 0);
    }
}

/*PWR button - 单击*/
static void on_pwr_single_click(Button* btn_handle)
{
    // ONLY set event bit - main task will handle everything else
    xEventGroupSetBits(pwr_groups, set_bit_button(0));
    if (s_button_evt_queue) {
        button_event_t evt = BTN_EVT_PWR_SINGLE;
        (void)xQueueSend(s_button_evt_queue, &evt, 0);
    }
}

/*PWR button - 长按*/
static void on_pwr_long_press(Button* btn_handle)
{
    // ONLY set event bit - main task will handle deep sleep entry
    xEventGroupSetBits(pwr_groups, set_bit_button(1));
    if (s_button_evt_queue) {
        button_event_t evt = BTN_EVT_PWR_LONG;
        (void)xQueueSend(s_button_evt_queue, &evt, 0);
    }
}

/*PWR button - double click*/
static void on_pwr_double_click(Button* btn_handle)
{
    // ONLY set event bit - main task will handle sound playback
    xEventGroupSetBits(pwr_groups, set_bit_button(2));
    if (s_button_evt_queue) {
        button_event_t evt = BTN_EVT_PWR_DOUBLE;
        (void)xQueueSend(s_button_evt_queue, &evt, 0);
    }
}

uint8_t user_button_get_repeat_count(void)
{
  	return (button_get_repeat_count(&button2));
}

uint8_t user_boot_get_repeat_count(void)
{
  	return (button_get_repeat_count(&button1));
}

bool user_buttons_are_idle(void)
{
    // Returns true only if BOTH buttons are in IDLE state
    // This means no button activity and no waiting for multi-click detection
    return (button1.state == BTN_STATE_IDLE && button2.state == BTN_STATE_IDLE);
}

void button_register_event_queue(QueueHandle_t queue)
{
    s_button_evt_queue = queue;
    if (queue) {
        ESP_LOGI(TAG, "Registered button event queue");
    } else {
        ESP_LOGW(TAG, "Button event queue cleared");
    }
}

/*
事件:
SINGLE_CLICK :单击
DOUBLE_CLICK :双击
PRESS_DOWN :按下
PRESS_UP :弹起事件
PRESS_REPEAT :重复按下
LONG_PRESS_START :长按触发一次
LONG_PRESS_HOLD :长按一直触发
*/



