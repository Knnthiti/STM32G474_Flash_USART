#include "Clock_system.h"

void SystemClock_Init()
{
    // Enable STM32 ART Accelerator (Adaptive Real-Time Memory Accelerator)
    LL_FLASH_EnablePrefetch();  // 1. Get ready
    LL_FLASH_EnableInstCache(); // 2. Remember code
    LL_FLASH_EnableDataCache(); // 3. Keep data clos

    LL_RCC_HSI_Enable();

    // Wait HSE oscillator is stable and ready to use
    while (!LL_RCC_HSI_IsReady())
    {
    }

    LL_RCC_HSE_Enable();

    // Wait HSE oscillator is stable and ready to use
    while (!LL_RCC_HSE_IsReady())
    {
    }

    // PLL Configurations (Target: 170 MHz SYSCLK from 24 MHz HSE)
    LL_UTILS_PLLInitTypeDef PLLInitStruct = {
        .PLLM = LL_RCC_PLLM_DIV_6, // HSE / 6 = 0.4 MHz
        .PLLN = 85,                // 24 MHz * 85 = 340 MHz (VCO output)
        .PLLR = LL_RCC_PLLR_DIV_2  // 340 MHz / 2 = 170 MHz (SYSCLK)
    };

    // Bus Clock Prescalers Configurations
    LL_UTILS_ClkInitTypeDef ClkInitStruct = {
        .AHBCLKDivider = LL_RCC_SYSCLK_DIV_1,
        .APB1CLKDivider = LL_RCC_APB1_DIV_2, // 170 / 2 = 85 MHz
        .APB2CLKDivider = LL_RCC_APB2_DIV_4  // 170 / 4 = 42 MHz
    };

    // Configure System Clock using
    if (LL_PLL_ConfigSystemClock_HSE(24000000, LL_UTILS_HSEBYPASS_OFF,
                                     &PLLInitStruct, &ClkInitStruct) != SUCCESS)
    {
        // Initialization Error
        while (1)
        {
        }
    }
		
		LL_Init1msTick(170000000);
}

void Button_PC13_Init(void) {
    LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOC);
    LL_GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = LL_GPIO_PIN_13;
    GPIO_InitStruct.Mode = LL_GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = LL_GPIO_PULL_NO;
    LL_GPIO_Init(GPIOC, &GPIO_InitStruct);
}

void LED_PA5_Init(void) {
    LL_AHB2_GRP1_EnableClock(LL_AHB2_GRP1_PERIPH_GPIOA);

    LL_GPIO_InitTypeDef GPIO_InitStruct = {0};
    GPIO_InitStruct.Pin = LL_GPIO_PIN_5;
    GPIO_InitStruct.Mode = LL_GPIO_MODE_OUTPUT; 
    GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_LOW;  
    GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL; 
    GPIO_InitStruct.Pull = LL_GPIO_PULL_NO;          
    
    LL_GPIO_Init(GPIOA, &GPIO_InitStruct);
}

static volatile uint32_t uwTick = 0;

void SystemTickConfig(uint32_t ticks, FunctionalState state)
{
    LL_InitTick(SystemCoreClock, ticks);

	if(state == ENABLE)
	{
		NVIC_SetPriority(SysTick_IRQn, 1U);
		SysTick->CTRL |= SysTick_CTRL_TICKINT_Msk;
	}
}

uint32_t GetTick(void)
{
    return uwTick;
}

void SysTick_Handler(void)
{
    uwTick++;
}
