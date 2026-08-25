#ifndef __FIRMWARE__H
#define __FIRMWARE__H

#define FIRMWARE_VERSION 0x10           // 0x10 as V.1.x Compatible Mode of  Ver.2.0 (22SR2,22L)
                                        // 0x20 for 22SR2 Srrict Mode
                                        // 0x28 for 22L   Strict Mode


//--- following definition is available in  Strict Mode: FIRMWARE_VERSION 0x20,0x28

#if ( (FIRMWARE_VERSION == 0x20) || (FIRMWARE_VERSION == 0x28))

#define RECEIVE_DATA_CHECK 1                 // Check CheckSum & Received Length In Strict Mode 



//--- for ver.2.x --- Definition of  REG09
//

//--- definition  do not change!!! below

#define StrictMode_EN   0b01000000      // Strict Mode Enable
#define RecvPKT_AD_EN   0b00100000      // Set Receive Packet Target address  into ReceiveData
#define SendPKT_CS_EN   0b00010000      // Set CheckSum at the last of SendData by MCU 
#define RecvPKT_CS_EN   0b00001000      // Set Checksum at the last of SendData by LoRa
#define RecvPKT_SIZE_EN 0b00000100      // Set PacketSize at the first of SendData by LoRa

#if   AUXLow_TIME == 0                  // be decided abobe AUXLow_TIME 
#define WaitTillWakeUpMCU 0b00000000    // 2-3 msec ( default )
#elif AUXLow_TIME == 1
#define WaitTillWakeUpMCU 0b00000001    // 512 msec
#elif AUXLow_TIME == 2
#define WaitTillWakeUpMCU 0b00000010    // 1024 msec
#elif AUXLow_TIME == 3
#define WaitTillWakeUpMCU 0b00000011    // 0 msec (No wait)
#else
#define WaitTillWakeUpMCU 0b00000000    // Else Default
#endif

//--- MACROS --- easy to see

#define IsNow_StrictMode      (REG09_Byte & StrictMode_EN) 
#define IsNow_RecvPKT_AD_EN   (REG09_Byte & RecvPKT_AD_EN)
#define IsNow_RecvPKT_CS_EN   (REG09_Byte & RecvPKT_CS_EN)
#define IsNow_SendPKT_CS_EN   (REG09_Byte & SendPKT_CS_EN)
#define IsNow_RecvPKT_SIZE_EN (REG09_Byte & RecvPKT_SIZE_EN)

#endif

#endif
