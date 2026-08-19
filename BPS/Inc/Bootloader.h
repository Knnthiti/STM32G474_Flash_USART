#ifndef BOOTLOADER_H
#define BOOTLOADER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32g4xx.h"
#include "stm32g4xx_ll_rcc.h"
#include "stm32g4xx_ll_crc.h"
#include "stm32g4xx_ll_dma.h"
#include "stm32g4xx_ll_gpio.h"
#include "stm32g4xx_ll_bus.h"

#include <string.h>
#include "Clock_system.h"
#include "USART.h"

#define FLASH_START_APP1 0x08003000

#define size_u32BufferProgram 1016
extern uint32_t u32BufferProgram[size_u32BufferProgram];
#define MaximumAddress_BufferFlash 4064


typedef enum{
   PC_CMD_START     = 0x07,
   PC_CMD_SENDING   = 0x67,
   PC_CMD_FINISHED  = 0x99,
   PC_CMD_WAIT      = 0x20,
	
   PC_CMD_START_PRI     = 0x08,
   PC_CMD_SENDING_PRI   = 0x68,
   PC_CMD_FINISHED_PRI  = 0x9A,
   PC_CMD_WAIT_PRI      = 0x21
}PC_Command_t;

typedef enum{
   STM_ACK_READY    = 0x49,
   STM_ACK_PAUSE    = 0x69,
   STM_ACK_FINISHED = 0x55,
	
   STM_ACK_READY_PRI    = 0x4A,
   STM_ACK_PAUSE_PRI    = 0x6A,
   STM_ACK_FINISHED_PRI = 0x56
}STM_ACK_t;

#define size_u8USARTdata 1024
#define size_u32USARTdata (size_u8USARTdata >> 2)

#define LPUART1_TX_DMA_CHANNEL LL_DMA_CHANNEL_4
#define LPUART1_RX_DMA_CHANNEL LL_DMA_CHANNEL_5
#define LPUART1_DMA_FRAME_SIZE size_u8USARTdata

typedef struct __attribute__((packed)){
    union {
        //(Header 4 + Buffer 1016 + CRC 4 = 1024 Bytes)
        struct {
            union {
                struct setting {
                    uint8_t u8herder;
                    uint8_t u8version;
                    uint16_t u16size;
                } u8setting1Byte;
                uint32_t u32setting4Byte;
            };
            
            uint32_t u32BufferData[254]; // 254 * 4 = 1016 Bytes
            
            union {
                uint32_t u32CRC4Byte;
                struct u8CRC {
                    uint8_t CRCByte0;
                    uint8_t CRCByte1;
                    uint8_t CRCByte2;
                    uint8_t CRCByte3;
                } CRC1Byte;
            };
        };

        uint8_t u8Data[size_u8USARTdata];
        uint32_t u32Data[size_u32USARTdata];
    };
}_USARTData;

extern volatile uint8_t __attribute__((section(".bss.Version_Program"))) Version_Edit;

void bootJumpToApp1(void);
void CRC_DmaInit(uint32_t u32MemAddr, uint32_t u32memLength);
uint32_t software_crc32(uint32_t *data, uint16_t length);
void CRC_APP_RX_DATA(uint32_t *buffer, uint32_t len);

uint8_t IsFlash_WaitForOperation(void);
void Clear_errorflags(void);

void Flash_Unlock(void);
void Flash_lock(void);
uint8_t IsFlash_lock(void);

uint8_t B1_Flash_erase_Page(uint8_t Page);
uint8_t B1_Erase_All_App(void);

typedef struct{
	union{
		uint32_t 	u32Buffer[8];
		uint16_t 	u16Buffer[16];
		uint8_t 	u8Buffer[32];
	};
}BufferFlash;

uint8_t B1_Flash_Write(uint32_t u32FlashAddress, uint32_t *u32Data32B, uint16_t u16DataCount);

extern volatile uint16_t current_program;
extern uint32_t timeoutStart;
extern volatile uint16_t g4_rxIndex;
extern volatile uint16_t g4_txIndex;

extern volatile _USARTData TX_USART_Data;
extern volatile _USARTData RX_USART_Data;
uint32_t GetTick(void);

#define USART_TIMEOUT_MS 10000

// State Machine Enum
typedef enum {
    G4_STATE_INIT_RX       = 0x00,
    G4_STATE_WAIT_RX       = 0x01,
    G4_STATE_PROCESS_DATA  = 0x02,
    G4_STATE_START_TX      = 0x03,
    G4_STATE_WAIT_TX       = 0x04,
    G4_STATE_TIMEOUT_RESET = 0x05
} G4_State_t;

extern volatile G4_State_t g4_currentState;

void PocessCommand_G4(void);
void Send_Data_LPUART1_DMA(uint32_t *buffer, uint32_t len);
void Receive_Data_LPUART1_DMA(uint32_t *buffer, uint32_t len);
void UART_ProcessState_G4(void);
void LPUART1_IRQHandler(void);
void DMA1_Channel4_IRQHandler(void);
void DMA1_Channel5_IRQHandler(void);

#ifdef __cplusplus
}
#endif

#endif /* BOOTLOADER_H */
