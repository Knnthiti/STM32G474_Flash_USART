#include "stm32g4xx_ll_bus.h"
#include "stm32g4xx_ll_gpio.h"
#include "stm32g4xx_ll_pwr.h"
#include "stm32g4xx_ll_rcc.h"
#include "stm32g4xx_ll_system.h"
#include "stm32g4xx_ll_utils.h"

void SystemClock_Init(void);
void Button_PC13_Init(void);
void LED_PA5_Init(void);

void SystemTickConfig(uint32_t ticks, FunctionalState state);
uint32_t GetTick(void);
void SysTick_Handler(void);
