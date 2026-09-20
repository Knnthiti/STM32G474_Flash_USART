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

/* Application image start address. The bootloader jumps to this vector table. */
#define FLASH_START_APP1 0x08003000

/* STM32G474 flash layout and packet sizing used by the firmware-update flow. */
#define FLASH_DUAL_BANK_PAGE_SIZE_BYTES   0x800U
#define FLASH_SINGLE_BANK_PAGE_SIZE_BYTES 0x1000U
#define FLASH_DUAL_BANK_PAGE_COUNT        128U
#define FLASH_APP_END_ADDRESS             0x08080000U
#define FLASH_BUSY_TIMEOUT_ITERATIONS     17000000UL
#define BOOTLOADER_PACKET_DATA_WORDS      254U

/* Flash status bits that must be checked/cleared after erase or program. */
#define FLASH_ERROR_FLAGS (FLASH_SR_FASTERR | FLASH_SR_MISERR  | FLASH_SR_PGSERR | \
                           FLASH_SR_SIZERR  | FLASH_SR_PGAERR  | FLASH_SR_WRPERR | \
                           FLASH_SR_PROGERR | FLASH_SR_OPERR   | FLASH_SR_RDERR  | \
                           FLASH_SR_OPTVERR)

/* RAM staging buffer. Each received data packet contributes 254 words. */
#define size_u32BufferProgram 1016
extern uint32_t u32BufferProgram[size_u32BufferProgram];
#define MaximumAddress_BufferFlash 4064

/* Commands received from the PC-side flasher. *_PRI is the active protocol set. */
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

/* ACK values returned to the PC after each command is processed. */
typedef enum{
   STM_ACK_READY    = 0x49,
   STM_ACK_PAUSE    = 0x69,
   STM_ACK_FINISHED = 0x55,
	
   STM_ACK_READY_PRI    = 0x4A,
   STM_ACK_PAUSE_PRI    = 0x6A,
   STM_ACK_FINISHED_PRI = 0x56
}STM_ACK_t;

/* Full UART/DMA frame size: header(4) + payload(1016) + CRC(4). */
#define size_u8USARTdata 1024
#define size_u32USARTdata (size_u8USARTdata >> 2)

/* DMA channel mapping for LPUART1 bootloader communication. */
#define LPUART1_TX_DMA_CHANNEL LL_DMA_CHANNEL_4
#define LPUART1_RX_DMA_CHANNEL LL_DMA_CHANNEL_5
#define LPUART1_DMA_FRAME_SIZE size_u8USARTdata

/*
 * Shared packet view used for both RX and TX.
 * The union lets the same 1024-byte frame be accessed as bytes, words,
 * header fields, payload words, and CRC.
 */
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

/* Last accepted firmware version byte from the PC packet header. */
extern volatile uint8_t __attribute__((section(".bss.Version_Program"))) Version_Edit;

/* Application jump and CRC helpers. */
void bootJumpToApp1(void);
void CRC_DmaInit(uint32_t u32MemAddr, uint32_t u32memLength);
uint32_t software_crc32(uint32_t *data, uint16_t length);
void CRC_APP_RX_DATA(uint32_t *buffer, uint32_t len);

/* Flash low-level helpers. */
uint8_t IsFlash_WaitForOperation(void);
void Clear_errorflags(void);

void Flash_Unlock(void);
void Flash_lock(void);
uint8_t IsFlash_lock(void);

uint8_t B1_Flash_erase_Page(uint16_t Page);
uint8_t B1_Erase_App(uint32_t firmwareBytes);

/* Flash erase diagnostics exposed for the debugger Watch window. */
extern volatile uint16_t g_flashErasePage;
extern volatile uint32_t g_flashLastStatus;
extern volatile uint8_t g_flashWaitTimedOut;

/* Small 32-byte scratch buffer used around flash program operations. */
typedef struct{
	union{
		uint32_t 	u32Buffer[8];
		uint16_t 	u16Buffer[16];
		uint8_t 	u8Buffer[32];
	};
}BufferFlash;

uint8_t B1_Flash_Write(uint32_t u32FlashAddress, uint32_t *u32Data32B, uint16_t u16DataCount);

/* Runtime counters for receive/program offsets and DMA progress. */
extern volatile uint16_t current_program;
extern uint32_t timeoutStart;
extern volatile uint16_t g4_rxIndex;
extern volatile uint16_t g4_txIndex;

/* Global UART frame buffers used by the bootloader state machine. */
extern volatile _USARTData TX_USART_Data;
extern volatile _USARTData RX_USART_Data;


#define USART_TIMEOUT_MS 10000

/* State machine states for packet receive, validation, command processing, and ACK TX. */
typedef enum {
    G4_STATE_INIT_RX       = 0x00,
    G4_STATE_WAIT_RX       = 0x01,
    G4_STATE_PROCESS_DATA  = 0x02,
    G4_STATE_START_TX      = 0x03,
    G4_STATE_WAIT_TX       = 0x04,
    G4_STATE_TIMEOUT_RESET = 0x05
} G4_State_t;

extern volatile G4_State_t g4_currentState;

/* Bootloader command processor, DMA frame I/O, and main polling state machine. */
void PocessCommand_G4(void);
void Send_Data_LPUART1_DMA(uint32_t *buffer, uint32_t len);
void Receive_Data_LPUART1_DMA(uint32_t *buffer, uint32_t len);
void UART_ProcessState_G4(void);


#ifdef __cplusplus
}
#endif

#endif /* BOOTLOADER_H */
