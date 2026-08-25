
#ifndef __ESP32_E220900T22S_JP_LIB__H
#define __ESP32_E220900T22S_JP_LIB__H

#include <vector>
#include <string>
#include <Arduino.h>
#include <SPIFFS.h>

#include "FIRMWARE.H"



// Set serial for debug console (to the Serial Monitor)
#define SerialMon Serial
// Set serial for LoRa (to the module)
#define SerialLoRa Serial2

// E220-900T22S(JP)のLoRa設定ファイル
#if (FIRMWARE_VERSION == 0x10)
#define CONFIG_FILENAME "/e220900t22s_jp_lora_config.ini"
#endif
#if (FIRMWARE_VERSION == 0x20)
#define CONFIG_FILENAME "/e220900t22s_jp_config_v20.ini"
#endif
#if (FIRMWARE_VERSION == 0x28)
#define CONFIG_FILENAME "/e220900t22s_jp_config_v28.ini"
#endif

// E220-900T22S(JP)へのピンアサイン
#define LoRa_ModeSettingPin_M0 4
#define LoRa_ModeSettingPin_M1 13
#define LoRa_RxPin 18
#define LoRa_TxPin 23
#define LoRa_AUXPin 34
#define LoRa_BaudRate 9600

// E220-900T22S(JP)の設定項目
struct LoRaConfigItem_t {
  uint16_t own_address;
  uint8_t baud_rate;
  uint8_t air_data_rate;
  uint8_t subpacket_size;
  uint8_t rssi_ambient_noise_flag;
  uint8_t transmitting_power;
  uint8_t own_channel;
  uint8_t rssi_byte_flag;
  uint8_t transmission_method_type;
  uint8_t wor_cycle;                 // ! notice proviously uint16_t
  uint16_t encryption_key;
  uint16_t target_address;
  uint8_t target_channel;
#if ( (FIRMWARE_VERSION == 0x20) || (FIRMWARE_VERSION == 0x28) )
  uint8_t strict_mode;
  uint8_t Lowvoltage_operation;
  uint8_t recv_target_address;
  uint8_t send_data_checksum;
  uint8_t recv_data_checksum;
  uint8_t recv_data_packetsize;
  uint8_t aux_low_holdtime;
  uint8_t carriresense_timeout;
#endif
#if FIRMWARE_VERSION == 0x28
  uint8_t transmitting_power_table;
#endif

};

struct RecvFrameE220900T22SJP_t {
  uint8_t recv_data[408];
  uint16_t recv_data_len;
  int rssi;
};


#if ( (FIRMWARE_VERSION == 0x20) || (FIRMWARE_VERSION == 0x28) )

struct LoRaRecvPacket_t {
  uint16_t  packet_size;
  uint16_t packet_address;
  uint8_t  *packet_data;
  int      packet_rssi;
  int      packet_error;
  uint8_t  recv_multi_pkt_f;
};

struct REGISTERVal_t {
  uint8_t response;
  uint8_t address;
  uint8_t bytes;
  uint8_t val[10];
  uint8_t error;
};

union REGUnion {
  uint8_t data[10];
  REGISTERVal_t REGUniStr;
};

#endif



class CLoRa {
  public:
    /**
       @brief E220-900T22S(JP)のLoRa設定値を読み込む
       @param filename 設定ファイル名
       @param config 読み込んだLoRa設定値の格納先
       @return 0:成功 1:失敗
    */
    int LoadConfigSetting(const char *filename, struct LoRaConfigItem_t &config);

    /**
       @brief E220-900T22S(JP)のLoRa設定値をデフォルトの値にセット
       @param config LoRa設定値の格納先
    */
    void SetDefaultConfigValue(struct LoRaConfigItem_t &config);

    /**
       @brief E220-900T22S(JP)へLoRa初期設定を行う
       @param config LoRa設定値の格納先
       @return 0:成功 1:失敗
    */
    int InitLoRaModule(struct LoRaConfigItem_t &config);

    /**
       @brief LoRa受信を行う
       @param
       @return 0:成功 1:失敗
    */
    int ReceiveFrame(struct RecvFrameE220900T22SJP_t *recv_frame);

    /**
       @brief LoRa送信を行う
       @param config LoRa設定値の格納先
       @param send_data 送信データ
       @param size 送信データサイズ
       @return 0:成功 1:失敗
    */
    int SendFrame(struct LoRaConfigItem_t &config, uint8_t *send_data, int size);


    void SwitchToNormalMode(void);           /** @brief ノーマルモード(M0=0,M1=0)へ移行する */

    void SwitchToWORSendingMode(void);       /** @brief WOR送信モード(M0=1,M1=0)へ移行する */

    void SwitchToWORReceivingMode(void);     /** @brief WOR受信モード(M0=0,M1=1)へ移行する */

    void SwitchToConfigurationMode(void);    /** @brief コンフィグモード(M0=1,M1=1)へ移行する */

    int init_REG09_0A_Byte();               /** strict mode  REG09, REG0A の初期化 */

    uint8_t ReadFirmwareVersion();           /** Read Firware version Return : VERSION NO */

    int CheckFirmwareVersion();              /** Return 0 : Firmware & DEVICE is Correct / 1: incorrect*/



    // ---　available in  Strict Mode: FIRMWARE_VERSION 0x20,0x28

#if ( (FIRMWARE_VERSION == 0x20) || (FIRMWARE_VERSION == 0x28) )


    /**
       @brief mode3 で書き込めるREG09,REG0Aレジスタの書込み
       @param str 書込みコマンド（REG09,REG0A指定）
              str_length コマンドバイト数
              SetData 1バイトデータ
       @return 0:成功 1:失敗
    */
    int Write_RegisterLIB(const char* str, int str_length, uint8_t SetData);

    /**
       @brief mode3 で読み込めるレジスタの読み込み
       @param address レジスタアドレス
              bytes 読み込むバイト数
       @return 0:成功 1:失敗
    */
    int Read_RegisterLIB( uint8_t address, uint8_t bytes);

    /**
       @brief strict mode において　mode0,mode2で読み込むレジスタの読み込み
       @param address レジスタアドレス
              bytes 読み込むバイト数
       ->　読み込み値の格納先　lora.REGData.data[3～]
       @return 0:成功 1:失敗
    */
    int Read_RegisterLIB_s( uint8_t address, uint8_t bytes);

    /**
       @brief 個体番号レジスタの読み込み
       @param RegNum  レジスタアドレス
       ->　読み込み値の格納先　lora.REGData.data[3]
       @return 0:成功 1:失敗
    */
    uint8_t read_id_num(char RegNum);

    /**
       @brief 個体番号レジスタへの書き込み
       @param RegNum  レジスタアドレス
              IDdata  データ
       @return 0:成功 1:失敗
    */
    uint8_t write_id_num(char RegNum, uint8_t IDdata);

    /**
       ※ Read_RegisterLIB, Read_Register_s, Write_RegisterLIBには実装済み
       @brief レジスタアクセス（読み書き）時に、UARTバッファのデータ有無をチェックし、読み込む
       @param receive_data LoRa受信データの格納先
       @return 0:データ有り 1:データ無し
               データ有りの場合：受信バッファ（receive_data LoRa受信データの格納先）
                            Buffered_ReceiveData = 1;
               データ無しの場合：Buffered_ReceiveData = 0;
    */

    int SetConfigStrictMode();               /** to set REG09 & REG0A of Strict mode */

    int Lowvoltage_operation_Enable();

    int Lowvoltage_operation_Disable();

    int StrictModeStart();                   /** to Set REG09 bit6 */

    int StrictModeEnd();                     /** to Reset REG09 bit6 */

    int ReceivePKTaddADRS();                 /** to Set REG09 bit5*/

    int ReceivePKTdelADRS();                 /** to Reset REG09 bit5*/

    int SendPKTaddCS();                      /** to Set REG09 bit4*/

    int SendPKTdelCS();                      /** to Reset REG09 bit4*/

    int ReceivePKTaddCS();                   /** to Set REG09 bit3*/

    int ReceivePKTdelCS();                   /** to Reset REG09 bit3*/

    int ReceivePKTaddSIZE();                 /** to Set REG09 bit2*/

    int ReceivePKTdelSIZE();                 /** to Reset REG09 bit2*/

    int REG09_ByteWrite();                   /** write uint8_t REG09_Byte into REG09 */

    int REG09_ByteWrite(uint8_t);            /** write argu into REG09  */

    int REG0A_ByteWrite();                   /** write uint8_t REG0A_Byte into REG0A */

    int REG09_Set_AUX_HoldTime(uint8_t bt);  /** write AUX Hold Low Time */

    int REG0A_Set_CarrierSenseTime(uint8_t ms); /** write AUX Hold Low Time */

    int Check_BufferedReceiveData_Exist();

#endif


    // ---  Data Definittion Area both ver.1.x and ver.2.x

    struct LoRaConfigItem_t config;           /** REG0 - REG8 defined Configuration table */
    struct RecvFrameE220900T22SJP_t data;     /** Received Air Data Area  */

    // --- only in Strict Mode
#if ( (FIRMWARE_VERSION == 0x20) || (FIRMWARE_VERSION == 0x28) )
    uint8_t conf_REG09_Byte;
    uint8_t conf_REG0A_Byte;
    uint8_t REG09_Byte;                       /** REG09 basis */
    uint8_t REG0A_Byte;                       /** REG0A basis */
    uint8_t Buffered_ReceiveData;

    union REGUnion REGData;                   /** Data Setting by ReadRegister */
    struct LoRaRecvPacket_t RcvPacket[20];    /** for Multi-packet in case of ver.2.0 */


#if 0            //  SAMPLE instead of   uint8_t REG09_Byte;
    typedef struct {
      unsigned int auxlevel : 2;
      unsigned int pktsize : 1;
      unsigned int rcvchksum : 1;
      unsigned int sndchksum : 1;
      unsigned int addren : 1;
      unsigned int strict : 1;
      unsigned int uvlo  : 1;
    } reg09bits_t;

    typedef union {
      unsigned char  byte;
      reg09bits_t    bit;
    } reg09union_t;

    reg09union_t REG09_Byte;
#endif

#endif



  private:
    uint16_t own_address_val;
    uint32_t baud_rate_val;
    uint16_t bw_val;
    uint8_t sf_val;
    uint8_t subpacket_size_val;
    uint8_t rssi_ambient_noise_flag_val;
    uint8_t transmitting_power_val;
    uint8_t own_channel_val;
    uint16_t wor_cycle_val;
    uint16_t encryption_key_val;
    uint16_t target_address_val;
    uint8_t target_channel_val;
    uint8_t transmission_method_type_val;
    uint8_t rssi_byte_flag_val;

    int OpenConfigFile(const char *filename, struct LoRaConfigItem_t &config);
    int ReadConfigValue(const char *key, const char *val);
    int SetConfigValue(struct LoRaConfigItem_t &config);
    void SetDefaultConfigValue_into_private_val();

#if ( (FIRMWARE_VERSION == 0x20) || (FIRMWARE_VERSION == 0x28) )
    int Skip_Receive_LoRa_Response();
    int CheckAndGetPacket(struct RecvFrameE220900T22SJP_t *recv_frame, int recvdata_end_ptr, uint8_t checksum, uint8_t previous_checksum, int arrayNo, int dataTop);
#endif

};


#define RECVstrc struct RecvFrameE220900T22SJP_t


#endif
