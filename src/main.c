#include "stm32f1xx_hal.h"

// --- STATE MACHINE & ENUMS ---
typedef enum {
    STATE_HOME_TIME,
    STATE_BRIGHTNESS_MENU,
    STATE_MODE_MENU,
    STATE_SET_HOURS,
    STATE_SET_MINS
} SystemState;

typedef enum {
    VIEW_12H,
    VIEW_24H,
    VIEW_SLOW
} ClockViewMode;

SystemState currentState = STATE_HOME_TIME;
ClockViewMode currentView = VIEW_12H;

// --- GLOBAL VARIABLES ---
uint8_t hours = 12, minutes = 0, seconds = 0;
uint8_t brightness_level = 20; // 1 to 20 (5% to 100%)
uint8_t manual_level = 4;      // Tracks L1 (1) through L4 (4)
uint8_t is_auto_brightness = 1; // 1 = Auto, 0 = Manual
uint32_t last_interaction_time = 0;
uint32_t last_second_tick = 0;
uint32_t last_adc_time = 0;

ADC_HandleTypeDef hadc1;

// --- CUSTOM 7-SEGMENT MAPPING ---
const uint8_t font[18] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F, // 0-9
    0x77, // 10: A
    0x73, // 11: P
    0x7C, // 12: b
    0x50, // 13: r
    0x1C, // 14: u
    0x6D, // 15: S
    0x38, // 16: L
    0x00  // 17: Blank
};

// --- DISPLAY DRIVER ---
void SetDigit1(uint8_t char_idx) {
    uint8_t bits = font[char_idx];
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, (bits & 0x01) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_14, (bits & 0x02) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5,  (bits & 0x04) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6,  (bits & 0x08) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_7,  (bits & 0x10) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_9,  (bits & 0x20) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8,  (bits & 0x40) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void SetDigit2(uint8_t char_idx) {
    uint8_t bits = font[char_idx];
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, (bits & 0x01) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, (bits & 0x02) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7, (bits & 0x04) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, (bits & 0x08) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1, (bits & 0x10) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_3, (bits & 0x20) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6, (bits & 0x40) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void RenderHomeTime() {
    uint8_t cycle_sec;
    uint8_t disp_h, disp_m, am_pm;
    
    disp_m = minutes;
    if (hours == 0) { disp_h = 12; am_pm = 10; } 
    else if (hours < 12) { disp_h = hours; am_pm = 10; }
    else if (hours == 12) { disp_h = 12; am_pm = 11; } 
    else { disp_h = hours - 12; am_pm = 11; }

    if (currentView == VIEW_12H) {
        cycle_sec = seconds % 15;
        if (cycle_sec < 6) { SetDigit1((disp_h / 10) ? (disp_h / 10) : 17); SetDigit2(disp_h % 10); }
        else if (cycle_sec < 12) { SetDigit1(disp_m / 10); SetDigit2(disp_m % 10); }
        else { SetDigit1(17); SetDigit2(am_pm); }
    }
    else if (currentView == VIEW_24H) {
        cycle_sec = seconds % 15;
        if (cycle_sec < 10) { SetDigit1(hours / 10); SetDigit2(hours % 10); }
        else { SetDigit1(disp_m / 10); SetDigit2(disp_m % 10); }
    }
    else if (currentView == VIEW_SLOW) {
        cycle_sec = seconds % 60;
        if (cycle_sec < 50) { SetDigit1((disp_h / 10) ? (disp_h / 10) : 17); SetDigit2(disp_h % 10); }
        else if (cycle_sec < 55) { SetDigit1(disp_m / 10); SetDigit2(disp_m % 10); }
        else { SetDigit1(17); SetDigit2(am_pm); }
    }
}

// --- BUTTON DEBOUNCING WITH GHOST-PRESS PREVENTION ---
uint8_t sw1_state = 0, sw2_state = 0;
uint32_t sw1_down_time = 0, sw2_down_time = 0;

static uint8_t both_pressed_handled = 0; 
static uint8_t ignore_sw1_release = 0;
static uint8_t ignore_sw2_release = 0;

void ProcessButtons() {
    uint8_t sw1_reading = (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_15) == GPIO_PIN_RESET);
    uint8_t sw2_reading = (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_12) == GPIO_PIN_RESET);
    uint32_t now = HAL_GetTick();

    // 1. Simultaneous Press Logic
    if (sw1_reading && sw2_reading) {
        if (!both_pressed_handled) {
            if (currentState == STATE_HOME_TIME) {
                currentState = STATE_SET_HOURS;
            } else if (currentState == STATE_SET_HOURS) {
                currentState = STATE_SET_MINS;
            } else if (currentState == STATE_SET_MINS) {
                currentState = STATE_HOME_TIME;
                seconds = 0; 
            }
            last_interaction_time = now;
            both_pressed_handled = 1; // Lock out multi-trigger
        }
        
        sw1_state = 1; 
        sw2_state = 1; 
        ignore_sw1_release = 1; // Block the ghost release
        ignore_sw2_release = 1; // Block the ghost release
        return; 
    } else if (!sw1_reading && !sw2_reading) {
        both_pressed_handled = 0; // Clear lockout only when both are physically let go
    }

    // 2. SW1 (Brightness / Decrease)
    if (sw1_reading && !sw1_state) { 
        sw1_down_time = now; 
        sw1_state = 1; 
        ignore_sw1_release = 0; // Fresh press
    }
    if (!sw1_reading && sw1_state) {
        sw1_state = 0;
        if (ignore_sw1_release) {
            ignore_sw1_release = 0; // It was a ghost release, do nothing
        } else if (now - sw1_down_time > 50) { 
            last_interaction_time = now;
            if (currentState == STATE_HOME_TIME) currentState = STATE_BRIGHTNESS_MENU;
            else if (currentState == STATE_BRIGHTNESS_MENU) {
                if (is_auto_brightness) { is_auto_brightness = 0; manual_level = 1; brightness_level = 1; } 
                else if (manual_level == 1) { manual_level = 2; brightness_level = 3; } 
                else if (manual_level == 2) { manual_level = 3; brightness_level = 8; } 
                else if (manual_level == 3) { manual_level = 4; brightness_level = 20; } 
                else { is_auto_brightness = 1; }
            }
            else if (currentState == STATE_SET_HOURS) { hours = (hours == 0) ? 23 : hours - 1; }
            else if (currentState == STATE_SET_MINS) { minutes = (minutes == 0) ? 59 : minutes - 1; }
        }
    }

    // 3. SW2 (Mode / Increase)
    if (sw2_reading && !sw2_state) { 
        sw2_down_time = now; 
        sw2_state = 1; 
        ignore_sw2_release = 0; // Fresh press
    }
    if (!sw2_reading && sw2_state) {
        sw2_state = 0;
        if (ignore_sw2_release) {
            ignore_sw2_release = 0; // It was a ghost release, do nothing
        } else if (now - sw2_down_time > 50) { 
            last_interaction_time = now;
            if (currentState == STATE_HOME_TIME) {
                currentState = STATE_MODE_MENU;
                if (currentView == VIEW_12H) currentView = VIEW_24H;
                else if (currentView == VIEW_24H) currentView = VIEW_SLOW;
                else currentView = VIEW_12H;
            }
            else if (currentState == STATE_SET_HOURS) { hours = (hours + 1) % 24; }
            else if (currentState == STATE_SET_MINS) { minutes = (minutes + 1) % 60; }
        }
    }
}

// --- HARDWARE INITIALIZATION ---
void SystemClock_Config(void) {
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
    HAL_RCC_OscConfig(&RCC_OscInitStruct);

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK|RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
    HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0);
}

void MX_GPIO_Init(void) {
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_AFIO_CLK_ENABLE();

    __HAL_AFIO_REMAP_SWJ_NOJTAG(); 

    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_3|GPIO_PIN_4|GPIO_PIN_5|GPIO_PIN_6|GPIO_PIN_7, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_4|GPIO_PIN_5|GPIO_PIN_6|GPIO_PIN_7|GPIO_PIN_8|GPIO_PIN_9, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13|GPIO_PIN_14, GPIO_PIN_RESET);

    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    GPIO_InitStruct.Pin = GPIO_PIN_3|GPIO_PIN_4|GPIO_PIN_5|GPIO_PIN_6|GPIO_PIN_7;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_4|GPIO_PIN_5|GPIO_PIN_6|GPIO_PIN_7|GPIO_PIN_8|GPIO_PIN_9;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_13|GPIO_PIN_14;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Pin = GPIO_PIN_15;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_12;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Pin = GPIO_PIN_2;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
}

void MX_ADC1_Init(void) {
    ADC_ChannelConfTypeDef sConfig = {0};
    __HAL_RCC_ADC1_CLK_ENABLE();
    hadc1.Instance = ADC1;
    hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
    hadc1.Init.ContinuousConvMode = DISABLE;
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
    hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
    hadc1.Init.NbrOfConversion = 1;
    HAL_ADC_Init(&hadc1);

    sConfig.Channel = ADC_CHANNEL_2;
    sConfig.Rank = ADC_REGULAR_RANK_1;
    sConfig.SamplingTime = ADC_SAMPLETIME_71CYCLES_5;
    HAL_ADC_ConfigChannel(&hadc1, &sConfig);
}

// --- MAIN LOOP ---
int main(void) {
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_ADC1_Init();

    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_0, GPIO_PIN_SET);

    while (1) {
        uint32_t now = HAL_GetTick();
        
        if (now - last_second_tick >= 1000) {
            last_second_tick = now;
            seconds++;
            if (seconds >= 60) {
                seconds = 0;
                minutes++;
                if (minutes >= 60) {
                    minutes = 0;
                    hours = (hours + 1) % 24;
                }
            }
        }

        // Auto Brightness Poll
        if (is_auto_brightness && (now - last_adc_time > 1000)) {
            last_adc_time = now;
            HAL_ADC_Start(&hadc1);
            if (HAL_ADC_PollForConversion(&hadc1, 10) == HAL_OK) {
                uint32_t adc_val = HAL_ADC_GetValue(&hadc1);
                
                // --- CALIBRATION TUNING VARIABLES ---
                // Higher ADC = Darker Room (Photoresistor resistance increases)
                uint32_t adc_dark = 3600;  // Threshold for minimum brightness (Room with main lights off)
                uint32_t adc_light = 3200; // Threshold for max brightness (Dim indoor lighting conditions)
                
                // Clamp ADC values to our window
                if (adc_val > adc_dark) adc_val = adc_dark;
                if (adc_val < adc_light) adc_val = adc_light;
                
                // Foolproof Linear Interpolation
                uint32_t range = adc_dark - adc_light;
                uint32_t inverted_val = adc_dark - adc_val; // Flips it so 0 is dark, 'range' is bright
                
                // Math output: 1 (min) to 20 (max)
                brightness_level = 1 + ((inverted_val * 19) / range);
            }
        }

        ProcessButtons();

        if ((currentState == STATE_BRIGHTNESS_MENU || currentState == STATE_MODE_MENU) 
            && (now - last_interaction_time > 5000)) {
            currentState = STATE_HOME_TIME;
        }

        switch (currentState) {
            case STATE_BRIGHTNESS_MENU:
                if (is_auto_brightness) { SetDigit1(10); SetDigit2(14); } // 'Au'
                else { SetDigit1(16); SetDigit2(manual_level); } // Exactly 'L1' to 'L4'
                break;

            case STATE_MODE_MENU:
                if (currentView == VIEW_12H) { SetDigit1(1); SetDigit2(2); }      
                else if (currentView == VIEW_24H) { SetDigit1(2); SetDigit2(4); } 
                else if (currentView == VIEW_SLOW) { SetDigit1(15); SetDigit2(16); } 
                break;

            case STATE_SET_HOURS:
                SetDigit1(hours / 10); SetDigit2(hours % 10);
                break;

            case STATE_SET_MINS:
                SetDigit1(minutes / 10); SetDigit2(minutes % 10);
                break;

            case STATE_HOME_TIME:
                RenderHomeTime();
                break;
        }
        
        HAL_Delay(10); 
    }
}

// --- SOFTWARE PWM (1kHz, 20 Steps) ---
volatile uint8_t pwm_tick = 0;
void SysTick_Handler(void) {
    HAL_IncTick();
    
    pwm_tick++;
    if (pwm_tick > 20) pwm_tick = 1;
    
    if (pwm_tick <= brightness_level) {
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_4, GPIO_PIN_SET);
    } else {
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_4, GPIO_PIN_RESET);
    }
}