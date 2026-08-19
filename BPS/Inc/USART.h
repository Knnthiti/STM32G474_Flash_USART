#ifndef USART_H
#define USART_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32g4xx_ll_bus.h"
#include "stm32g4xx_ll_dma.h"
#include "stm32g4xx_ll_dmamux.h"
#include "stm32g4xx_ll_gpio.h"
#include "stm32g4xx_ll_lpuart.h"
#include "stm32g4xx_ll_rcc.h"
#include "stm32g4xx_ll_usart.h"
#include "stm32g4xx_ll_utils.h"

#define SLAVE1_ADDRESS 0x01U
#define SLAVE2_ADDRESS 0x02U

void UART5_DMA_Init(void);
void UART5_UART_Init(void);

void LPUART1_DMA_Init(void);
void LPUART1_UART_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* USART_H */
