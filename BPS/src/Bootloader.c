#include "Bootloader.h"


/* Jump from bootloader context to the application reset handler. */
void bootJumpToApp1(){ 
	
	// Define a function pointer type for the application reset handler.
	typedef int (*pFunction)(void);
	static pFunction vJumpToApp;
	
	// Read the reset handler address from the application vector table.
	vJumpToApp	= (pFunction)(*(__IO uint32_t*)(FLASH_START_APP1 + 4));

	// Load the application stack pointer before jumping to application code.
	__set_MSP(*(__IO uint32_t*)FLASH_START_APP1);
	
	// Disable interrupts so bootloader interrupts do not run inside the application.
	__disable_irq();
	
	// Branch to the application reset handler.
	vJumpToApp();
}

uint32_t u32CrcCalculateLength;

/* Configure CRC peripheral and DMA channel 1 for memory-to-CRC transfers. */
void CRC_DmaInit(uint32_t u32MemAddr, uint32_t u32memLength) {
    
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_CRC);
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMAMUX1); 

    LL_CRC_ResetCRCCalculationUnit(CRC);
    LL_CRC_SetPolynomialSize(CRC, LL_CRC_POLYLENGTH_32B);
    LL_CRC_SetInputDataReverseMode(CRC, LL_CRC_INDATA_REVERSE_NONE);
    LL_CRC_SetOutputDataReverseMode(CRC, LL_CRC_OUTDATA_REVERSE_NONE);
    LL_CRC_SetInitialData(CRC, 0xFFFFFFFF);
    LL_CRC_SetPolynomialCoef(CRC, 0x04C11DB7);

    LL_DMA_DisableChannel(DMA1, LL_DMA_CHANNEL_1);

    LL_DMA_SetDataTransferDirection(DMA1, LL_DMA_CHANNEL_1, LL_DMA_DIRECTION_MEMORY_TO_MEMORY);
    LL_DMA_SetChannelPriorityLevel(DMA1, LL_DMA_CHANNEL_1, LL_DMA_PRIORITY_HIGH);
    LL_DMA_SetMode(DMA1, LL_DMA_CHANNEL_1, LL_DMA_MODE_NORMAL);
    
    LL_DMA_SetPeriphIncMode(DMA1, LL_DMA_CHANNEL_1, LL_DMA_PERIPH_INCREMENT);
    LL_DMA_SetMemoryIncMode(DMA1, LL_DMA_CHANNEL_1, LL_DMA_MEMORY_NOINCREMENT);
    
    LL_DMA_SetPeriphSize(DMA1, LL_DMA_CHANNEL_1, LL_DMA_PDATAALIGN_WORD);
    LL_DMA_SetMemorySize(DMA1, LL_DMA_CHANNEL_1, LL_DMA_MDATAALIGN_WORD);

    LL_DMA_ConfigAddresses(DMA1, LL_DMA_CHANNEL_1, 
                           u32MemAddr,             // Source: RAM
                           (uint32_t)&(CRC->DR),   // Destination: Hardware CRC
                           LL_DMA_DIRECTION_MEMORY_TO_MEMORY);
    
    LL_DMA_SetDataLength(DMA1, LL_DMA_CHANNEL_1, u32memLength);

    LL_DMA_ClearFlag_TC1(DMA1);

    LL_DMA_EnableChannel(DMA1, LL_DMA_CHANNEL_1);

    while(LL_DMA_IsActiveFlag_TC1(DMA1) == 0) {

    }

    LL_DMA_ClearFlag_TC1(DMA1);
    LL_DMA_DisableChannel(DMA1, LL_DMA_CHANNEL_1);
}

/* Software CRC32 implementation matching the STM32 CRC peripheral settings. */
uint32_t software_crc32(uint32_t *data, uint16_t length) {
    uint32_t crc = 0xFFFFFFFF;

    for (uint16_t i = 0; i < length; i++) {
		
		crc ^= data[i];

        for (uint8_t j = 0; j < 32; j++) {
            if (crc & 0x80000000) {
                crc = (crc << 1) ^ 0x04C11DB7;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

volatile uint32_t My_CRC = 0;
volatile uint32_t My_CRC_SW = 0;
volatile uint32_t Received_CRC = 67;

/* Calculate CRC for the received packet body and load the packet CRC field. */
void CRC_APP_RX_DATA(uint32_t *buffer, uint32_t len) {
    My_CRC_SW = software_crc32(buffer, (uint16_t)len);
    My_CRC = My_CRC_SW;
    Received_CRC = RX_USART_Data.u32CRC4Byte; 
}

/* Return 1 while flash is busy, otherwise return 0. */
uint8_t IsFlash_WaitForOperation(void){
	return((FLASH->SR & FLASH_SR_BSY) ? 1 : 0);
}

volatile uint16_t g_flashErasePage = 0xFFFFU;
volatile uint32_t g_flashLastStatus = 0U;
volatile uint8_t g_flashWaitTimedOut = 0U;

/* Prevent a failed Flash operation from trapping the bootloader forever. */
static uint8_t Flash_WaitUntilReady(void)
{
	uint32_t timeout = FLASH_BUSY_TIMEOUT_ITERATIONS;

	while(IsFlash_WaitForOperation()){
		if(timeout-- == 0U){
			g_flashWaitTimedOut = 1U;
			g_flashLastStatus = FLASH->SR;
			return 1U;
		}
	}

	return 0U;
}

/* Clear end-of-operation and all tracked flash error flags. */
void Clear_errorflags(void){
	FLASH->SR = (FLASH_SR_EOP | FLASH_ERROR_FLAGS);
}

/* Unlock flash control registers before erase/program operations. */
void Flash_Unlock(void){
  FLASH->KEYR = 0x45670123;
  FLASH->KEYR = 0xCDEF89AB;
}

/* Lock flash control registers after protected flash access is no longer needed. */
void Flash_lock(void){
  FLASH->CR |= FLASH_CR_LOCK;
}

/* Return 1 when flash control registers are locked. */
uint8_t IsFlash_lock(void){
	return(FLASH->CR & FLASH_CR_LOCK) ? 1 : 0;
}


/* Erase one flash page, selecting bank 1 or bank 2 from the absolute page number. */
uint8_t B1_Flash_erase_Page(uint16_t Page){
	uint32_t pageInBank = Page;
	uint8_t dualBank = ((FLASH->OPTR & FLASH_OPTR_DBANK) != 0U) ? 1U : 0U;
	
	g_flashErasePage = Page;
	g_flashLastStatus = 0U;
	g_flashWaitTimedOut = 0U;

	if(Flash_WaitUntilReady() != 0U){
		return 1U;
	}
	
	//2.Unlock Flash
	if(IsFlash_lock()){
        Flash_Unlock();
    }
	if(IsFlash_lock()){
		g_flashLastStatus = FLASH->CR;
		return 1U;
	}
	
	//1.Clear all the error flags
	Clear_errorflags();
	
	FLASH->CR &= ~(FLASH_CR_EOPIE_Msk | FLASH_CR_PG | FLASH_CR_MER1 | FLASH_CR_MER2);
	
	if((dualBank != 0U) && (Page >= FLASH_DUAL_BANK_PAGE_COUNT)){
		pageInBank = (uint32_t)Page - FLASH_DUAL_BANK_PAGE_COUNT;
		FLASH->CR |= FLASH_CR_BKER;
	}else{
		FLASH->CR &= ~FLASH_CR_BKER_Msk;
	}

	if(pageInBank >= FLASH_DUAL_BANK_PAGE_COUNT){
		g_flashLastStatus = FLASH_SR_SIZERR;
		return 1U;
	}
	
	//Set PER for choose erase Page mode.
	FLASH->CR |= FLASH_CR_PER;
	
	//Clear sector
	FLASH->CR &= ~FLASH_CR_PNB_Msk;
	//4.Set PNB for target sector.
	FLASH->CR |= pageInBank << FLASH_CR_PNB_Pos;
	
	//5.Set START1
	FLASH->CR |= FLASH_CR_STRT;
	
	//Wait to finish erase
	if(Flash_WaitUntilReady() != 0U){
		return 1U;
	}
	
	//clear CR->SER for Out erase sector mode.
	FLASH->CR &= ~(FLASH_CR_PER_Msk | FLASH_CR_PNB_Msk | FLASH_CR_BKER_Msk);
	
	if((FLASH->SR & FLASH_ERROR_FLAGS) != 0U){
		g_flashLastStatus = FLASH->SR;
		Clear_errorflags();
		return 1;
	}
	
	//clear EOP
	FLASH->SR = FLASH_SR_EOP; 	 
	return 0;
}

/* Erase only the pages required by the incoming application image. */
uint8_t B1_Erase_App(uint32_t firmwareBytes){
	uint32_t pageSize;
	uint32_t appCapacity = FLASH_APP_END_ADDRESS - FLASH_START_APP1;
	uint32_t packetBytes = BOOTLOADER_PACKET_DATA_WORDS * sizeof(uint32_t);
	uint32_t programBytes;
	uint16_t totalPages;
	uint16_t startPage;
	uint16_t erasePages;
	uint16_t endPage;

	if((firmwareBytes == 0U) || (firmwareBytes > appCapacity)){
		g_flashLastStatus = FLASH_SR_SIZERR;
		return 1U;
	}

	/* The final packet is padded and all 1016 payload bytes are programmed. */
	programBytes = ((firmwareBytes + packetBytes - 1U) / packetBytes) * packetBytes;
	if(programBytes > appCapacity){
		g_flashLastStatus = FLASH_SR_SIZERR;
		return 1U;
	}

	pageSize = ((FLASH->OPTR & FLASH_OPTR_DBANK) != 0U) ?
	           FLASH_DUAL_BANK_PAGE_SIZE_BYTES : FLASH_SINGLE_BANK_PAGE_SIZE_BYTES;

	if(((FLASH_START_APP1 - FLASH_BASE) % pageSize) != 0U){
		g_flashLastStatus = FLASH_SR_PGAERR;
		return 1U;
	}

	totalPages = (uint16_t)((FLASH_APP_END_ADDRESS - FLASH_BASE) / pageSize);
	startPage = (uint16_t)((FLASH_START_APP1 - FLASH_BASE) / pageSize);
	erasePages = (uint16_t)((programBytes + pageSize - 1U) / pageSize);
	endPage = startPage + erasePages;
	
	if(endPage > totalPages){
		g_flashLastStatus = FLASH_SR_SIZERR;
		return 1U;
	}
	
	for(uint16_t i = startPage ; i < endPage ; i++){
		if(B1_Flash_erase_Page(i) != 0U){
			return 1;
		}
	}
	
	return 0;
}

BufferFlash _32byteBufferFlash;

/* Program the staged words into flash using STM32 double-word programming. */
uint8_t B1_Flash_Write(uint32_t u32FlashAddress, uint32_t *u32Data32B, uint16_t u16DataCount){
	uint32_t programBytes;
	uint32_t endAddress;
	
	if(u16DataCount == 0U){
		return 0;
	}
	
	if((u32FlashAddress < FLASH_START_APP1) || ((u32FlashAddress & 0x7U) != 0U)){
		return 1;
	}
	
	programBytes = (((uint32_t)u16DataCount + 1U) & ~1UL) * 4;
	endAddress = u32FlashAddress + programBytes;
	
	if((endAddress < u32FlashAddress) || (endAddress > FLASH_APP_END_ADDRESS)){
		return 1;
	}
	
	g_flashLastStatus = 0U;
	g_flashWaitTimedOut = 0U;
	if(Flash_WaitUntilReady() != 0U){
		return 1U;
	}
	
	//2.Unlock Flash
	if(IsFlash_lock()){
        Flash_Unlock();
    }
	if(IsFlash_lock()){
		g_flashLastStatus = FLASH->CR;
		return 1U;
	}
	
	//1.Clear all the error flags
	Clear_errorflags();
	
	//Clear Buffer
	for(uint8_t i = 0; i < 8 ; i++){
		_32byteBufferFlash.u32Buffer[i] = 0xFFFFFFFF;
	}
	
	for(uint16_t i = 0U ; i < u16DataCount ; i += 2U){
		uint32_t word0 = u32Data32B[i];
		uint32_t word1 = u32Data32B[i + 1U];
		uint32_t writeAddress = u32FlashAddress + ((uint32_t)i * 4);
		
		//3.Set SER1 for choose Write mode.
		FLASH->CR |= FLASH_CR_PG;
		
		*(__IO uint32_t *)writeAddress = word0;
		__ISB();// Using instruction barier to make sure that be write a flash with the correct order.
		*(__IO uint32_t *)(writeAddress + 4U) = word1;
		
		if(Flash_WaitUntilReady() != 0U){
			return 1U;
		}
		
		//clear CR->PG for Out Write mode.
		FLASH->CR &= ~FLASH_CR_PG_Msk;
		
		if((FLASH->SR & FLASH_ERROR_FLAGS) != 0U){
			g_flashLastStatus = FLASH->SR;
			Clear_errorflags();
			return 1;
		}
		
		//clear EOP
		FLASH->SR = FLASH_SR_EOP; 
	}
	
	return 0;
}

/* Packet staging buffer and global RX/TX protocol frames. */
uint32_t __attribute__((aligned(4))) u32BufferProgram[size_u32BufferProgram] ={0};

volatile _USARTData TX_USART_Data;
volatile _USARTData RX_USART_Data;

volatile uint8_t __attribute__((section(".bss.Version_Program"))) Version_Edit;

uint32_t u32offset_FlashAddress = 0;
volatile uint16_t current_program = 0;

uint32_t timeoutStart = 0;
volatile uint16_t g4_rxIndex = 0;
volatile uint16_t g4_txIndex = 0;
volatile G4_State_t g4_currentState = G4_STATE_INIT_RX;

/* Reset the whole staging buffer to the erased-flash value. */
void Bootloader_ClearProgramBuffer(void)
{
	memset(u32BufferProgram, 0xFF, sizeof(u32BufferProgram));
}

/* Build a 1024-byte ACK frame header for the PC-side flasher. */
void Bootloader_PrepareAck(STM_ACK_t ack)
{
    /* Clear in place: a 1024-byte temporary would exceed the stack budget. */
    memset((void *)&TX_USART_Data, 0, sizeof(TX_USART_Data));
    TX_USART_Data.u8setting1Byte.u8herder = (uint8_t)ack;
    TX_USART_Data.u8setting1Byte.u8version = Version_Edit;
    TX_USART_Data.u8setting1Byte.u16size = size_u8USARTdata;
}

/* Write any staged data packet words to flash and advance the app offset. */
uint8_t Bootloader_FlashProgramBuffer(void)
{
    uint32_t flashAddress;
    uint32_t byteCount;
	
    if(current_program == 0U){
        return 0;
    }
	
    flashAddress = FLASH_START_APP1 + u32offset_FlashAddress;
    byteCount = (uint32_t)current_program * 4;
	
    if(B1_Flash_Write(flashAddress, u32BufferProgram, current_program) != 0U){
        return 1;
    }
	
    u32offset_FlashAddress += byteCount;
    current_program = 0U;
    Bootloader_ClearProgramBuffer();
	
    return 0;
}

// ==========================================
// 2. Command Processing Function
// ==========================================
void PocessCommand_G4(void) 
{
    uint8_t status = 0U;
	
    // Process command based on the received header
    switch (RX_USART_Data.u8setting1Byte.u8herder)
    {
        case PC_CMD_START_PRI: { // 0x08
            /* Start a new firmware update: erase app area and reset write offsets. */
			uint32_t firmwareBytes;

			memcpy(&firmwareBytes,
			       (const void *)&RX_USART_Data.u8Data[sizeof(uint32_t)],
			       sizeof(firmwareBytes));
            __disable_irq();
            status = B1_Erase_App(firmwareBytes); // Erase only pages used by this image.
            __enable_irq(); 
            
            current_program = 0;
            u32offset_FlashAddress = 0;
            Version_Edit = RX_USART_Data.u8setting1Byte.u8version;
            Bootloader_ClearProgramBuffer();
            
            if(status == 0U){
                Bootloader_PrepareAck(STM_ACK_READY_PRI); // 0x4A
                Send_Data_LPUART1_DMA((uint32_t*)TX_USART_Data.u32Data, size_u8USARTdata);
            }
            break;
        }

        case PC_CMD_SENDING_PRI: // 0x68
            /* Let the PC continue until the RAM staging buffer is full. */
            if (current_program < size_u32BufferProgram) {
                Bootloader_PrepareAck(STM_ACK_READY_PRI); // 0x4A
                Send_Data_LPUART1_DMA((uint32_t*)TX_USART_Data.u32Data, size_u8USARTdata);
            } else {
                Bootloader_PrepareAck(STM_ACK_PAUSE_PRI); // 0x6A
                Send_Data_LPUART1_DMA((uint32_t*)TX_USART_Data.u32Data, size_u8USARTdata);
            }
            break;

        case PC_CMD_WAIT_PRI: // 0x21
            /* PC paused transmission; commit the current staging buffer to flash. */
            __disable_irq();
            status = Bootloader_FlashProgramBuffer();
            __enable_irq();
            
            if(status == 0U){
                Bootloader_PrepareAck(STM_ACK_READY_PRI); // 0x4A
                Send_Data_LPUART1_DMA((uint32_t*)TX_USART_Data.u32Data, size_u8USARTdata);
            }
            break;

        case PC_CMD_FINISHED_PRI: // 0x9A
            /* No more packets are coming; flush remaining data and report success. */
            __disable_irq();
            status = Bootloader_FlashProgramBuffer();
            __enable_irq();
            
            if(status == 0U){
                Version_Edit = RX_USART_Data.u8setting1Byte.u8version;
                current_program = 0;
                u32offset_FlashAddress = 0; 
                Bootloader_ClearProgramBuffer();
                Bootloader_PrepareAck(STM_ACK_FINISHED_PRI); // 0x56
                Send_Data_LPUART1_DMA((uint32_t*)TX_USART_Data.u32Data, size_u8USARTdata);
            }
            break;

        default:
            break;
    }
}

// ==========================================
// 3. Bootloader State Machine Function
// ==========================================
// Command LPUART1 to Send Data via DMA (Uses Channel 4)
/* Start a DMA transfer that sends one complete ACK frame through LPUART1. */
void Send_Data_LPUART1_DMA(uint32_t *buffer, uint32_t len) 
{
    uint32_t byteLength = len;
	
    // 1. Stop DMA channel before making any changes
    LL_LPUART_DisableDMAReq_TX(LPUART1);
    LL_DMA_DisableChannel(DMA1, LPUART1_TX_DMA_CHANNEL);
    while(LL_DMA_IsEnabledChannel(DMA1, LPUART1_TX_DMA_CHANNEL)){
    }
    
    // 2. Set the starting address of the data in RAM
    LL_DMA_SetMemoryAddress(DMA1, LPUART1_TX_DMA_CHANNEL, (uint32_t)buffer);
    
    // 3. Set peripheral address to LPUART1 Transmit Register
    LL_DMA_SetPeriphAddress(DMA1, LPUART1_TX_DMA_CHANNEL, LL_LPUART_DMA_GetRegAddr(LPUART1, LL_LPUART_DMA_REG_DATA_TRANSMIT));
    
    // 4. Set how many bytes to send
    LL_DMA_SetDataLength(DMA1, LPUART1_TX_DMA_CHANNEL, byteLength);
    
    // 5. Clear Transfer Complete and Error flags for Channel 4 BEFORE enabling
    LL_DMA_ClearFlag_TC4(DMA1); 
    LL_DMA_ClearFlag_TE4(DMA1);
    LL_LPUART_ClearFlag_TC(LPUART1);
    g4_txIndex = 0U;
    
    // 6. Enable DMA request in LPUART1 peripheral
    LL_LPUART_EnableDMAReq_TX(LPUART1);
    
    // 7. Start DMA to send the data
    LL_DMA_EnableChannel(DMA1, LPUART1_TX_DMA_CHANNEL);
}

// Command LPUART1 to Receive Data via DMA (Uses Channel 5)
/* Arm DMA reception for one complete 1024-byte packet from the PC. */
void Receive_Data_LPUART1_DMA(uint32_t *buffer, uint32_t len) 
{
    // 1. Stop DMA channel before making any changes
    LL_LPUART_DisableDMAReq_RX(LPUART1);
    LL_DMA_DisableChannel(DMA1, LPUART1_RX_DMA_CHANNEL);
    while(LL_DMA_IsEnabledChannel(DMA1, LPUART1_RX_DMA_CHANNEL)){
    }
    
    // 2. Tell DMA to save the incoming data into this buffer
    LL_DMA_SetMemoryAddress(DMA1, LPUART1_RX_DMA_CHANNEL, (uint32_t)buffer);
    
    // 3. Set peripheral address to LPUART1 Receive Register
    LL_DMA_SetPeriphAddress(DMA1, LPUART1_RX_DMA_CHANNEL, LL_LPUART_DMA_GetRegAddr(LPUART1, LL_LPUART_DMA_REG_DATA_RECEIVE));
    
    // 4. Set how many bytes we want to receive
    LL_DMA_SetDataLength(DMA1, LPUART1_RX_DMA_CHANNEL, len);
    
    // 5. Clear Transfer Complete and Error flags for Channel 5 BEFORE enabling
    LL_DMA_ClearFlag_TC5(DMA1); 
    LL_DMA_ClearFlag_TE5(DMA1);
    LL_LPUART_ClearFlag_IDLE(LPUART1);
    g4_rxIndex = 0U;
	
	 // 7. Start DMA and put it in standby mode to wait for data
    LL_DMA_EnableChannel(DMA1, LPUART1_RX_DMA_CHANNEL);
    
    // 6. Enable DMA request in LPUART1 peripheral
    LL_LPUART_EnableIT_IDLE(LPUART1);
    LL_LPUART_EnableDMAReq_RX(LPUART1);
}

/* Polling state machine that receives, validates, processes, and ACKs packets. */
void UART_ProcessState_G4(void) 
{
    switch (g4_currentState) {
        /* --- STATE: Initialize RX DMA --- */
        case G4_STATE_INIT_RX:
            // Command DMA to wait for incoming data
            /* Clear in place rather than creating two packet-sized temporaries. */
            memset((void *)&RX_USART_Data, 0, sizeof(RX_USART_Data));
            memset((void *)&TX_USART_Data, 0, sizeof(TX_USART_Data));
            Receive_Data_LPUART1_DMA((uint32_t*)RX_USART_Data.u8Data, size_u8USARTdata);
            timeoutStart = GetTick();
            g4_currentState = G4_STATE_WAIT_RX;
            break;

        /* --- STATE: Wait for Data --- */
        case G4_STATE_WAIT_RX:
            // Check if DMA RX (Channel 5) is complete
            if (LL_DMA_IsActiveFlag_TC5(DMA1)) {
                LL_DMA_ClearFlag_TC5(DMA1);
                g4_currentState = G4_STATE_PROCESS_DATA;
            }else if((GetTick() - timeoutStart) > USART_TIMEOUT_MS){
                g4_currentState = G4_STATE_TIMEOUT_RESET;
            }
            break;

        /* --- STATE: Process Received Data --- */
        case G4_STATE_PROCESS_DATA:
            // 1. Calculate CRC
            CRC_APP_RX_DATA((uint32_t*)RX_USART_Data.u32Data, size_u32USARTdata - 1U);
			
            if((My_CRC != Received_CRC) || 
               (RX_USART_Data.u8setting1Byte.u16size != size_u8USARTdata)){
                g4_currentState = G4_STATE_INIT_RX;
                break;
            }
            
            // 2. If CRC is correct and it is a SENDING command, copy data to buffer
            if(RX_USART_Data.u8setting1Byte.u8herder == PC_CMD_SENDING_PRI){
                if(current_program <= (size_u32BufferProgram - BOOTLOADER_PACKET_DATA_WORDS)){
                    /* RX DMA is complete in this state, so the payload is stable. */
                    memcpy(&u32BufferProgram[current_program],
                           (const void *)&RX_USART_Data.u8Data[sizeof(uint32_t)],
                           BOOTLOADER_PACKET_DATA_WORDS * sizeof(uint32_t));
                    current_program += BOOTLOADER_PACKET_DATA_WORDS;
                } 
            }

            // 3. Execute flash commands and send ACK
            PocessCommand_G4();
            
            // Clear header to avoid processing the same command twice
            RX_USART_Data.u8setting1Byte.u8herder = 0x00;
            
            if(TX_USART_Data.u8setting1Byte.u8herder != 0U){
                timeoutStart = GetTick();
                g4_currentState = G4_STATE_WAIT_TX;
            }else{
                g4_currentState = G4_STATE_INIT_RX;
            }
            break;

        /* --- STATE: Wait for TX Completion --- */
        case G4_STATE_WAIT_TX:
            // Check if DMA TX (Channel 4) is complete
            if (LL_DMA_IsActiveFlag_TC4(DMA1)) {
                LL_DMA_ClearFlag_TC4(DMA1);
                LL_LPUART_DisableDMAReq_TX(LPUART1);
                LL_DMA_DisableChannel(DMA1, LPUART1_TX_DMA_CHANNEL);
                g4_currentState = G4_STATE_INIT_RX; // Go back to receive next packet
            }else if((GetTick() - timeoutStart) > USART_TIMEOUT_MS){
                g4_currentState = G4_STATE_TIMEOUT_RESET;
            }
            break;

        /* --- STATE: Handle Timeout & Reset --- */
        case G4_STATE_TIMEOUT_RESET:
            LL_LPUART_DisableDMAReq_RX(LPUART1);
            LL_LPUART_DisableDMAReq_TX(LPUART1);
            LL_DMA_DisableChannel(DMA1, LPUART1_RX_DMA_CHANNEL); // Stop RX
            LL_DMA_DisableChannel(DMA1, LPUART1_TX_DMA_CHANNEL); // Stop TX
            LL_DMA_ClearFlag_TC5(DMA1);
            LL_DMA_ClearFlag_TE5(DMA1);
            LL_DMA_ClearFlag_TC4(DMA1);
            LL_DMA_ClearFlag_TE4(DMA1);
            g4_currentState = G4_STATE_INIT_RX;
            break;

        default:
            g4_currentState = G4_STATE_INIT_RX;
            break;
    }
}

void LPUART1_IRQHandler(void)
{
    /* IDLE interrupt extends the receive timeout while an incomplete frame is arriving. */
    if((LL_LPUART_IsActiveFlag_IDLE(LPUART1) != 0U) &&
       (LL_LPUART_IsEnabledIT_IDLE(LPUART1) != 0U)){
        uint32_t remaining;
		
        LL_LPUART_ClearFlag_IDLE(LPUART1);
        remaining = LL_DMA_GetDataLength(DMA1, LPUART1_RX_DMA_CHANNEL);
		
        if(remaining <= LPUART1_DMA_FRAME_SIZE){
            g4_rxIndex = (uint16_t)(LPUART1_DMA_FRAME_SIZE - remaining);
        }
		
        if((g4_currentState == G4_STATE_WAIT_RX) && (remaining == 0U)){
            LL_DMA_ClearFlag_TC5(DMA1);
            g4_currentState = G4_STATE_PROCESS_DATA;
        }else if(g4_currentState == G4_STATE_WAIT_RX){
            timeoutStart = GetTick();
        }
    }
	
    /* Any UART receive error forces the DMA/USART state machine to restart cleanly. */
    if((LL_LPUART_IsEnabledIT_ERROR(LPUART1) != 0U) &&
       ((LL_LPUART_IsActiveFlag_ORE(LPUART1) != 0U) ||
        (LL_LPUART_IsActiveFlag_FE(LPUART1) != 0U) ||
        (LL_LPUART_IsActiveFlag_NE(LPUART1) != 0U) ||
        (LL_LPUART_IsActiveFlag_PE(LPUART1) != 0U))){
        LL_LPUART_ClearFlag_ORE(LPUART1);
        LL_LPUART_ClearFlag_FE(LPUART1);
        LL_LPUART_ClearFlag_NE(LPUART1);
        LL_LPUART_ClearFlag_PE(LPUART1);
        g4_currentState = G4_STATE_TIMEOUT_RESET;
    }
}

/* RX DMA interrupt: complete packet received or transfer error occurred. */
void DMA1_Channel5_IRQHandler(void)
{
    if(LL_DMA_IsActiveFlag_TE5(DMA1) != 0U){
        LL_DMA_ClearFlag_TE5(DMA1);
        g4_currentState = G4_STATE_TIMEOUT_RESET;
    }
	
    if(LL_DMA_IsActiveFlag_TC5(DMA1) != 0U){
        LL_DMA_ClearFlag_TC5(DMA1);
        if(g4_currentState == G4_STATE_WAIT_RX){
            g4_rxIndex = LPUART1_DMA_FRAME_SIZE;
            g4_currentState = G4_STATE_PROCESS_DATA;
        }
    }
}

/* TX DMA interrupt: ACK frame sent or transfer error occurred. */
void DMA1_Channel4_IRQHandler(void)
{
    if(LL_DMA_IsActiveFlag_TE4(DMA1) != 0U){
        LL_DMA_ClearFlag_TE4(DMA1);
        g4_currentState = G4_STATE_TIMEOUT_RESET;
    }
	
    if(LL_DMA_IsActiveFlag_TC4(DMA1) != 0U){
        LL_DMA_ClearFlag_TC4(DMA1);
        LL_LPUART_DisableDMAReq_TX(LPUART1);
        LL_DMA_DisableChannel(DMA1, LPUART1_TX_DMA_CHANNEL);
        g4_txIndex = LPUART1_DMA_FRAME_SIZE;
		
        if(g4_currentState == G4_STATE_WAIT_TX){
            g4_currentState = G4_STATE_INIT_RX;
        }
    }
}
