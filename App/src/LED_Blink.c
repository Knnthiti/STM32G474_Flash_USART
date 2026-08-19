#include "Clock_system.h"

int main(){
  __enable_irq();

  SystemClock_Init();
	LED_PA5_Init();
  while(1){
	  LL_GPIO_SetOutputPin(GPIOA ,LL_GPIO_PIN_5);
		LL_mDelay(1000);
		LL_GPIO_ResetOutputPin(GPIOA ,LL_GPIO_PIN_5);
		LL_mDelay(1000);
	}
}