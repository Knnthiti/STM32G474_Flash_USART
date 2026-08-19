#include "main.h"

uint32_t Part_Time = 0;


int main(void) 
{
    // 1. Initialize System Clock and Tick Timer (for GetTick)
    SystemClock_Init();
    SystemTickConfig(1000, ENABLE);
    
    // 2. Initialize LPUART1 and its DMA channels
    LPUART1_UART_Init();

    Button_PC13_Init();
    // 3. Main Infinite Loop
    while(1) 
    {
        if ((GetTick() - Part_Time) > 10) {
            Part_Time = GetTick();
            // Let the state machine handle receiving, processing, and sending
            if(LL_GPIO_IsInputPinSet(GPIOC, LL_GPIO_PIN_13) == 1){
                bootJumpToApp1();
            }else{
                UART_ProcessState_G4();
            }
        }
    }    
}
