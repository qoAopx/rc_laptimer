/**
  Author Company:
    CLEALINK TECHNOLOGY Co.,Ltd.

  Description:
        Device            :  ESP32
        Library Version   :  2.0
*/

#include  "esp32_e220900t22s_jp_lib_v2.h"

//--- USE Serial Monitor or not / for Debugging
#define USE_SERIAL_PRINT 1

//MACROS
#if  USE_SERIAL_PRINT
#define Print Serial.printf
#define PrintLn Serial.println
#else
#define Print \
  // No Operation
#define PrintLn \
  // No Operation
#endif

// ------------------------------


SemaphoreHandle_t xMutex;

template<typename T>
bool ConfRange(T target, T min, T max);

int CLoRa::LoadConfigSetting(const char *filename, struct LoRaConfigItem_t &config) {
  int ret = 0;

  if (!SPIFFS.begin(true)) {
    Print("SPIFFS Mount Failed.\n");
  }

  // 最初にデフォルト値をセット
  SetDefaultConfigValue(config);

  // ESP32内にコンフィグファイルがあるか否か
  if (!SPIFFS.exists(filename)) {
    Print("config file is NOT exists.\n");
    ret = 1;
  } else {
    Print("\n Read config file %s.\n", filename);


    // コンフィグ値を設定
    // 値範囲外や読取エラー時はデフォルト値使用
    ret = OpenConfigFile(filename, config);
  }

  return ret;
}

int CLoRa::CheckFirmwareVersion() {
  uint8_t RetNo;

  RetNo = ReadFirmwareVersion();
  if (FIRMWARE_VERSION != RetNo ) return 1;

  return 0;
}


int CLoRa::InitLoRaModule(struct LoRaConfigItem_t &config) {
  int ret = 0;
  uint8_t checksum = 0;
  uint8_t ch;

  xMutex = xSemaphoreCreateMutex();
  xSemaphoreGive(xMutex);

  // コンフィグモード(M0=1,M1=1)へ移行する
  Print("switch to configuration mode\n");
  SwitchToConfigurationMode();

  pinMode(LoRa_AUXPin, INPUT);
  const unsigned long started = millis();
  while (digitalRead(LoRa_AUXPin) == LOW) {
    if (millis() - started > 1000) {
      return 2;  // AUX timeout
    }
    delay(1);
  }
  delay(50);

  SerialLoRa.begin(LoRa_BaudRate, SERIAL_8N1, LoRa_RxPin, LoRa_TxPin);

  // Configuration
  std::vector<uint8_t> command = { 0xc0, 0x00, 0x08 };
  std::vector<uint8_t> response = {};

  // Register Address 00H, 01H
  uint8_t ADDH = config.own_address >> 8;
  uint8_t ADDL = config.own_address & 0xff;
  command.push_back(ADDH);
  command.push_back(ADDL);

  // Register Address 02H
  uint8_t REG0 = 0;
  REG0 = REG0 | (config.baud_rate << 5);
  REG0 = REG0 | (config.air_data_rate);
  command.push_back(REG0);

  // Register Address 03H
  uint8_t REG1 = 0;
  REG1 = REG1 | (config.subpacket_size << 6);
  REG1 = REG1 | (config.rssi_ambient_noise_flag << 5);
  REG1 = REG1 | (config.transmitting_power);
  command.push_back(REG1);

  // Register Address 04H
  uint8_t REG2 = config.own_channel;
  command.push_back(REG2);

  // Register Address 05H
  uint8_t REG3 = 0;
  REG3 = REG3 | (config.rssi_byte_flag << 7);
  REG3 = REG3 | (config.transmission_method_type << 6);
#if FIRMWARE_VERSION == 0x28
  REG3 = REG3 | (config.transmitting_power_table << 5);
#endif
  REG3 = REG3 | (config.wor_cycle);
  command.push_back(REG3);

  // Register Address 06H, 07H
  uint8_t CRYPT_H = config.encryption_key >> 8;
  uint8_t CRYPT_L = config.encryption_key & 0xff;
  command.push_back(CRYPT_H);
  command.push_back(CRYPT_L);



  Print("# Command Request\n");
  for (auto i : command) {
    ch = i;
    Print("0x%02x ", i);
    checksum ^= ch;
  }
  Print("\n");
#if ( (FIRMWARE_VERSION == 0x20) || (FIRMWARE_VERSION == 0x28) )
  if (IsNow_SendPKT_CS_EN) command.push_back(checksum);
#endif
  if (xSemaphoreTake(xMutex, (portTickType)100) == pdTRUE) {
    for (auto i : command) {
      SerialLoRa.write(i);
    }

    SerialLoRa.flush();
    xSemaphoreGive(xMutex);
  }

  // delay(100);  // ★旧：AUXを見ない固定待ちのため応答タイミングがシビアだと取りこぼす

  // ★修正：AUXがHIGH（モジュールの処理完了）に戻るまで待つ。
  //   タイムアウトを設けて、万一AUXが機能しない配線でもフリーズしないようにする。
  unsigned long auxWaitStart = millis();
  while (digitalRead(LoRa_AUXPin) == LOW) {
    if (millis() - auxWaitStart > 1000) {
      Print("AUX wait timeout in InitLoRaModule()\n");
      break;
    }
    delay(2);
  }
  delay(20);  // AUXがHIGHに戻った直後の追加安定待ち（データシート推奨）

  while (SerialLoRa.available()) {
    if (xSemaphoreTake(xMutex, (portTickType)100) == pdTRUE) {
      uint8_t data = SerialLoRa.read();
      response.push_back(data);
      xSemaphoreGive(xMutex);
    }
  }
  Print("# Command Response\n");

  for (auto i : response) {
    Print("0x%02x ", i);
  }
  Print("\n");

  if (response.size() != command.size()) {
    ret = 1;
  }

  return ret;
}

#if FIRMWARE_VERSION < 0x20

// --- the beginning of ReceiveFlame // ver.1.x

int CLoRa::ReceiveFrame(struct RecvFrameE220900T22SJP_t *recv_frame) {
  int len = 0;
  uint8_t *start_p = recv_frame->recv_data;

  memset(recv_frame->recv_data, 0x00, sizeof(recv_frame->recv_data) / sizeof(recv_frame->recv_data[0]));

  while (1) {

    while (SerialLoRa.available()) {
      if (xSemaphoreTake(xMutex, (portTickType)100) == pdTRUE) {
        uint8_t ch = SerialLoRa.read();
        Print("%02x ", ch);

        *(start_p + len) = ch;

        len++;
        xSemaphoreGive(xMutex);
      }

      if (len > 408) {
        return 1;
      }
    }

    if ((SerialLoRa.available() == 0) && (len > 0)) {
      delay(10);
      if (SerialLoRa.available() == 0) {
        recv_frame->recv_data_len = len;
        if ( config.rssi_byte_flag ){
          recv_frame->recv_data_len -= 1;
          recv_frame->rssi = recv_frame->recv_data[len - 1] - 256;
        }
        break;
      }
    }
    delay(100);
  }
  Print("\nReceived Length = %d", recv_frame->recv_data_len );

  return 0;
}

#endif

// --- the end of ReceiveFlame // ver.1.x



int CLoRa::SendFrame(struct LoRaConfigItem_t &config, uint8_t *send_data, int size) {
  uint8_t subpacket_size = 0;

  //while (!LoRa_AUXPin);
  while (digitalRead(LoRa_AUXPin) == LOW) {
    delay(10);  // AUXがHIGH（バッファ空）になるまで待つ
  }
  //  Print("\n.......... send data length = %d \n", size);

  switch (config.subpacket_size) {
    case 0b00:
      subpacket_size = 200;
      break;
    case 0b01:
      subpacket_size = 128;
      break;
    case 0b10:
      subpacket_size = 64;
      break;
    case 0b11:
      subpacket_size = 32;
      break;
    default:
      subpacket_size = 200;
      break;
  }
  if (size > subpacket_size) {
    Print("send data length too long\n");
    return 1;
  }
  uint8_t target_address_H = config.target_address >> 8;
  uint8_t target_address_L = config.target_address & 0xff;
  uint8_t target_channel = config.target_channel;

  if (config.transmission_method_type == 0) size -= 3;  // -3 is to declare num of bytes of frame

  uint8_t frame[3 + size] = { target_address_H, target_address_L, target_channel };

  if (config.transmission_method_type == 1) {
    memmove(frame + 3, send_data, size);
  } else {
    memmove(frame, send_data, size + 3);                // +3 is to collect num of bytes
  }


#if 0 /* print debug */
  for (int i = 0; i < 3 + size; i++) {
    if (i < 3) {
      Print("%02x", frame[i]);
    } else {
      Print("%c", frame[i]);
    }
  }
  Print("\n");
#endif

  char checksum = 0;
  if (xSemaphoreTake(xMutex, (portTickType)100) == pdTRUE) {
    for (auto i : frame) {
      SerialLoRa.write(i);
      checksum ^= i;
    }
#if ( (FIRMWARE_VERSION == 0x20) || (FIRMWARE_VERSION == 0x28))
    if ( IsNow_SendPKT_CS_EN )  SerialLoRa.write(checksum);
#endif
    SerialLoRa.flush();
    delay(100);
    while (SerialLoRa.available()) {
      while (SerialLoRa.available()) {
        SerialLoRa.read();
      }
      delay(100);
    }
    xSemaphoreGive(xMutex);
  }
#if ( (FIRMWARE_VERSION == 0x20) || (FIRMWARE_VERSION == 0x28))
  if ( IsNow_SendPKT_CS_EN )  Print("\n...Send Checksum = %02x", checksum);
#endif
  return 0;
}

void CLoRa::SwitchToNormalMode(void) {
  pinMode(LoRa_ModeSettingPin_M0, OUTPUT);
  pinMode(LoRa_ModeSettingPin_M1, OUTPUT);

  digitalWrite(LoRa_ModeSettingPin_M0, 0);
  digitalWrite(LoRa_ModeSettingPin_M1, 0);
  delay(100);
}

void CLoRa::SwitchToWORSendingMode(void) {
  pinMode(LoRa_ModeSettingPin_M0, OUTPUT);
  pinMode(LoRa_ModeSettingPin_M1, OUTPUT);

  digitalWrite(LoRa_ModeSettingPin_M0, 1);
  digitalWrite(LoRa_ModeSettingPin_M1, 0);
  delay(100);
}

void CLoRa::SwitchToWORReceivingMode(void) {
  pinMode(LoRa_ModeSettingPin_M0, OUTPUT);
  pinMode(LoRa_ModeSettingPin_M1, OUTPUT);

  digitalWrite(LoRa_ModeSettingPin_M0, 0);
  digitalWrite(LoRa_ModeSettingPin_M1, 1);
  delay(100);
}

void CLoRa::SwitchToConfigurationMode(void) {
  pinMode(LoRa_ModeSettingPin_M0, OUTPUT);
  pinMode(LoRa_ModeSettingPin_M1, OUTPUT);

  digitalWrite(LoRa_ModeSettingPin_M0, 1);
  digitalWrite(LoRa_ModeSettingPin_M1, 1);
  delay(100);
}

// priority definition A (1 - 13)

void CLoRa::SetDefaultConfigValue_into_private_val(){
    own_address_val = 0x0000;         // 1
    baud_rate_val = 9600;             // 2
    bw_val = 125;                     // 3
    sf_val = 9;                       // +3
    subpacket_size_val = 200;         // 4
    rssi_ambient_noise_flag_val = 1;  // 5
    transmitting_power_val = 13;      // 6
    own_channel_val = 0x0;            // 7
    rssi_byte_flag_val = 1;           // 8
    transmission_method_type_val = 1; // 9
    wor_cycle_val = 2000;             //10
    encryption_key_val = 0;           //11

    target_address_val = 0x0000;      //12
    target_channel_val = 0x0;         //13
}

void CLoRa::SetDefaultConfigValue(struct LoRaConfigItem_t &config) {
  
// E220-900T22S/L Default definition B (1 - 13)

  const LoRaConfigItem_t default_config = {
    0x0000,   // 1 own_address 0
    0b011,    // 2 baud_rate 9600 bps
    0b10000,  // 3 air_data_rate SF:9 BW:125
    0b00,     // 4 subpacket_size 200
    0b0,      // 5 rssi_ambient_noise_flag 無効
    0b01,     // 6 transmitting_power 13 dBm
    0x0,      // 7 own_channel 0
    0b0,      // 8 rssi_byte_flag 無効
    0b1,      // 9 transmission_method_type 固定送信モード
    0b011,    //10 wor_cycle 2000 ms
    0x0000,   //11 encryption_key 0

    0x0000,   //12 target_address 0
    0x00,     //13 target_channel 0
 
#if ( (FIRMWARE_VERSION == 0x20) || (FIRMWARE_VERSION == 0x28) )
    0b1,      // strict_mode
    0b0,      // Lowvoltage_operation enable = 0
    0b0,      // recv_target_address disable
    0b0,      // send_data_checksum disable
    0b0,      // recv_data_checksum disable
    0b1,      // recv_data_packetsize enable
    0b00,     // aux_low_holdtime 00 default:2-3msec
    0x00,     // carriresense_timeout 00 (default)
#endif
#if FIRMWARE_VERSION == 0x28
    0b0      // transmitting_power_table table A
#endif
  };

  config = default_config;
  
  // 設定値をセットする   default definition A ( 1 - 13 )を　優先
  SetDefaultConfigValue_into_private_val();
  SetConfigValue(config);
}

int CLoRa::OpenConfigFile(const char *filename, struct LoRaConfigItem_t &config) {
  int ret = 0;

  // フラッシュメモリのファイルを開く
  File file = SPIFFS.open(filename, FILE_READ);
  if (!file) {
    Print("file open failed.\n");
    return 1;
  }

  while (file.available()) {
    int i;
    String key = file.readStringUntil('=');
    
    // Skip CR,LF and <'A' 
      for ( i = 0; i < sizeof(key) ; i++) if( key[i] >= 'A') break;
      if ( i != 0 ) key.remove(0, i);

    String val = file.readStringUntil('\n');

        Print("\n%s", key.c_str());
        Print(" = %s", val.c_str());

    key.toLowerCase();
    val.toLowerCase();

    // 設定値を内部変数に読み取る
    ReadConfigValue(key.c_str(), val.c_str());

  }
  PrintLn('\n');

  // 設定値をセットする
  SetConfigValue(config);

  return ret;
}




#if ( (FIRMWARE_VERSION == 0x20) || (FIRMWARE_VERSION == 0x28) )

//MACROS
#if  USE_SERIAL_PRINT
#define Putb_p putb
#else
#define Putb_p \
  // No Operation
#endif


#define CHAR_BIT 8

//extern uint8_t RSSINoise[10];
void printb(uint8_t dat) {
  unsigned int mask = (int)1 << (sizeof(dat) * CHAR_BIT - 1);
  do Serial.write(mask & dat ? '1' : '0');
  while (mask >>= 1);
}

void putb(uint8_t dat) {
  Serial.write('0'), Serial.write('b'), printb(dat);
}


// Keywords for ver.2.x

const char ENABLE[] = "enable";
const char DISABLE[] = "disable";
const char WrongSpell[] = "\nWrong Spell not \"enable\" or \"disable\" at \"";

const char STRICT_MODE[] = "strict_mode";
const char LOWVOLTAGE_OP[] = "lowvoltage_operation";
const char RECV_ADD[] = "recv_target_address";
const char SEND_CHKSUM[] = "send_data_checksum";
const char RECV_CHKSUM[] = "recv_data_checksum";
const char RECV_DATA_SIZE[] = "recv_data_packetsize";
const char AUX_HOLD[] = "aux_low_holdtime";
const char CARRIRESENSE[] = "carriresense_timeout";
#endif


int CLoRa::ReadConfigValue(const char *key, const char *val) {
  int number = 0;
  int err = 0;

  if (strcmp(key, "own_address") == 0) {
    number = atoi(val);
    own_address_val = number;
  } else if (strcmp(key, "baud_rate") == 0) {
    number = atoi(val);
    baud_rate_val = number;
  } else if (strcmp(key, "bw") == 0) {
    number = atoi(val);
    bw_val = number;
  } else if (strcmp(key, "sf") == 0) {
    number = atoi(val);
    sf_val = number;
  } else if (strcmp(key, "subpacket_size") == 0) {
    number = atoi(val);
    subpacket_size_val = number;
  } else if (strcmp(key, "transmitting_power") == 0) {
    number = atoi(val);
    transmitting_power_val = number;
  } else if (strcmp(key, "own_channel") == 0) {
    number = atoi(val);
    own_channel_val = number;
  } else if (strcmp(key, "wor_cycle") == 0) {
    number = atoi(val);
    wor_cycle_val = number;
  } else if (strcmp(key, "encryption") == 0) {
    number = atoi(val);
    encryption_key_val = number;
  } else if (strcmp(key, "target_address") == 0) {
    number = atoi(val);
    target_address_val = number;
  } else if (strcmp(key, "target_channel") == 0) {
    number = atoi(val);
    target_channel_val = number;
  } else if (strcmp(key, "transmission_method_type") == 0) {
    number = atoi(val);
    transmission_method_type_val = number;
  } else if (strcmp(key, "rssi_byte_flag") == 0) {
    number = atoi(val);
    rssi_byte_flag_val = number;
  } else if (strcmp(key, "rssi_ambient_noise_flag") == 0) {
    number = atoi(val);
    rssi_ambient_noise_flag_val = number;
  }
  
#if ( (FIRMWARE_VERSION == 0x20) || (FIRMWARE_VERSION == 0x28) )
  else if (strcmp(key, STRICT_MODE) == 0) {
    if (strcmp(val, ENABLE) == 0) {
      config.strict_mode = 0b1;
    } else if (strcmp(val, DISABLE) == 0) {
      config.strict_mode = 0b0;
    } else {
      Print(WrongSpell);
      Print("%s\"\n", key);
    }
  } else if (strcmp(key, LOWVOLTAGE_OP) == 0) {
    if (strcmp(val, ENABLE) == 0) {
      config.Lowvoltage_operation = 0b0;
    } else if (strcmp(val, DISABLE) == 0) {
      config.Lowvoltage_operation = 0b1;
    } else {
      Print(WrongSpell);
      Print("%s\"\n", key);
    }
  } else if (strcmp(key, RECV_ADD) == 0) {
    if (strcmp(val, ENABLE) == 0) {
      config.recv_target_address = 0b1;
    } else if (strcmp(val, DISABLE) == 0) {
      config.recv_target_address = 0b0;
    } else {
      Print(WrongSpell);
      Print("%s\"\n", key);
    }
  } else if (strcmp(key, SEND_CHKSUM) == 0) {
    if (strcmp(val, ENABLE) == 0) {
      config.send_data_checksum = 0b1;
    } else if (strcmp(val, DISABLE) == 0) {
      config.send_data_checksum = 0b0;
    } else {
      Print(WrongSpell);
      Print("%s\"\n", key);
    }
  } else if (strcmp(key, RECV_CHKSUM) == 0) {
    if (strcmp(val, ENABLE) == 0) {
      config.recv_data_checksum = 0b1;
    } else if (strcmp(val, DISABLE) == 0) {
      config.recv_data_checksum = 0b0;
    } else {
      Print(WrongSpell);
      Print("%s\"\n", key);
    }
  } else if (strcmp(key, RECV_DATA_SIZE) == 0) {
    if (strcmp(val, ENABLE) == 0) {
      config.recv_data_packetsize = 0b1;
    } else if (strcmp(val, DISABLE) == 0) {
      config.recv_data_packetsize = 0b0;
    } else {
      Print(WrongSpell);
      Print("%s\"\n", key);
    }
  } else if (strcmp(key, AUX_HOLD) == 0) {
    number = atoi(val);
    switch (number) {
      case 2:
      case 3:
        config.aux_low_holdtime = 0b00;
        break;
      case 512:
        config.aux_low_holdtime = 0b01;
        break;
      case 1024:
        config.aux_low_holdtime = 0b10;
        break;
      case 0:
        config.aux_low_holdtime = 0b11;
        break;
      default:
        config.aux_low_holdtime = 0b00;       // default 2 or 3
        Print("%s invalid value.\n", AUX_HOLD);
        Print("default %s value is used.\n", AUX_HOLD);
        break;
    }
  } else if (strcmp(key, CARRIRESENSE) == 0) {
    number = atoi(val);
    if ( number >= 256 ) {
      Print("%s invalid value.\n", CARRIRESENSE);
      Print("default %s value is used.\n", CARRIRESENSE);
      config.carriresense_timeout = 0;
    } else {
      config.carriresense_timeout = number;
    }
  }
#endif


  return err;
}

// コンフィグ値が設定範囲内か否か
template<typename T>
bool ConfRange(T target, T min, T max) {
  if (target >= min && target <= max) {
    return true;
  } else {
    return false;
  }
}

int CLoRa::SetConfigValue(struct LoRaConfigItem_t &config) {
  int err = 0;

  // own_address
  if (ConfRange((int)own_address_val, 0, 65535)) {
    config.own_address = own_address_val;
  } else {
    err = 1;
    Print("own_address invalid value.\n");
    Print("default own_address value is used.\n");
  }

  // baud_rate
  switch (baud_rate_val) {
    case 1200:
      config.baud_rate = 0b000;
      break;
    case 2400:
      config.baud_rate = 0b001;
      break;
    case 4800:
      config.baud_rate = 0b010;
      break;
    case 9600:
      config.baud_rate = 0b011;
      break;
    case 19200:
      config.baud_rate = 0b100;
      break;
    case 38400:
      config.baud_rate = 0b101;
      break;
    case 57600:
      config.baud_rate = 0b110;
      break;
    case 115200:
      config.baud_rate = 0b111;
      break;
    default:
      err = 1;
      Print("baud_rate invalid value.\n");
      Print("default baud_rate value is used.\n");
      break;
  }

  // air_data_rate
  switch (bw_val) {
    case 125:
      switch (sf_val) {
        case 5:
          config.air_data_rate = 0b00000;
          break;
        case 6:
          config.air_data_rate = 0b00100;
          break;
        case 7:
          config.air_data_rate = 0b01000;
          break;
        case 8:
          config.air_data_rate = 0b01100;
          break;
        case 9:
          config.air_data_rate = 0b10000;
          break;
        default:
          err = 1;
          Print("sf invalid value.\n");
          Print("default sf value is used.\n");
          break;
      }
      break;
    case 250:
      switch (sf_val) {
        case 5:
          config.air_data_rate = 0b00001;
          break;
        case 6:
          config.air_data_rate = 0b00101;
          break;
        case 7:
          config.air_data_rate = 0b01001;
          break;
        case 8:
          config.air_data_rate = 0b01101;
          break;
        case 9:
          config.air_data_rate = 0b10001;
          break;
        case 10:
          config.air_data_rate = 0b10101;
          break;
        default:
          err = 1;
          Print("sf invalid value.\n");
          Print("default sf value is used.\n");
          break;
      }
      break;
    case 500:
      switch (sf_val) {
        case 5:
          config.air_data_rate = 0b00010;
          break;
        case 6:
          config.air_data_rate = 0b00110;
          break;
        case 7:
          config.air_data_rate = 0b01010;
          break;
        case 8:
          config.air_data_rate = 0b01110;
          break;
        case 9:
          config.air_data_rate = 0b10010;
          break;
        case 10:
          config.air_data_rate = 0b10110;
          break;
        case 11:
          config.air_data_rate = 0b11010;
          break;
        default:
          err = 1;
          Print("sf invalid value.\n");
          Print("default sf value is used.\n");
          break;
      }
      break;
    default:
      err = 1;
      Print("bw invalid value.\n");
      Print("default bw and sf value is used.\n");
      break;
  }

  // subpacket_size
  switch (subpacket_size_val) {
    case 200:
      config.subpacket_size = 0b00;
      break;
    case 128:
      config.subpacket_size = 0b01;
      break;
    case 64:
      config.subpacket_size = 0b10;
      break;
    case 32:
      config.subpacket_size = 0b11;
      break;
    default:
      err = 1;
      Print("subpacket_size invalid value.\n");
      Print("default subpacket_size value is used.\n");
      break;
  }

  // transmitting_power
  switch (transmitting_power_val) {
    case 13:
      config.transmitting_power = 0b01;
      break;
    case 7:
      config.transmitting_power = 0b10;
      break;
    case 0:
      config.transmitting_power = 0b11;
      break;

    default:
#if FIRMWARE_VERSION == 0x20
      if ( (transmitting_power_val >= 1 ) && (transmitting_power_val <= 12 )) {
        config.transmitting_power = transmitting_power_val + 3;
        break;
      }
#endif

#if FIRMWARE_VERSION == 0x28
      if ( (transmitting_power_val >= 1 ) && (transmitting_power_val <= 22 )) {
        if ( transmitting_power_val & 0b00010000 )config.transmitting_power_table = 0b00;
        else                                      config.transmitting_power_table = 0b01;
        config.transmitting_power = (transmitting_power_val ^ 0b00001111) & 0b00001111;
        break;
      }
#endif
      err = 1;
      Print("transmitting_power invalid value.\n");
      Print("default transmitting_power value is used.\n");
      break;
  }

  // own_channel
  switch (bw_val) {
    case 125:
      if (ConfRange((int)own_channel_val, 0, 37)) {
        config.own_channel = own_channel_val;
      } else {
        err = 1;
        Print("own_channel invalid value.\n");
        Print("default own_channel value is used.\n");
      }
      break;
    case 250:
      if (ConfRange((int)own_channel_val, 0, 36)) {
        config.own_channel = own_channel_val;
      } else {
        err = 1;
        Print("own_channel invalid value.\n");
        Print("default own_channel value is used.\n");
      }
      break;
    case 500:
      if (ConfRange((int)own_channel_val, 0, 30)) {
        config.own_channel = own_channel_val;
      } else {
        err = 1;
        Print("own_channel invalid value.\n");
        Print("default own_channel value is used.\n");
      }
      break;
    default:
      err = 1;
      Print("bw invalid value.\n");
      Print("default own_channel value is used.\n");
      break;
  }

  // wor_cycle
  switch (wor_cycle_val) {
    case 500:
      config.wor_cycle = 0b000;
      break;
    case 1000:
      config.wor_cycle = 0b001;
      break;
    case 1500:
      config.wor_cycle = 0b010;
      break;
    case 2000:
      config.wor_cycle = 0b011;
      break;
    case 2500:
      config.wor_cycle = 0b100;
      break;
    case 3000:
      config.wor_cycle = 0b101;
      break;
    default:
      err = 1;
      Print("wor_cycle invalid value.\n");
      Print("default wor_cycle value is used.\n");
      break;
  }
  // encryption_key
  if (ConfRange((int)encryption_key_val, 0, 65535)) {
    config.encryption_key = encryption_key_val;
  } else {
    err = 1;
    Print("encryption_key invalid value.\n");
    Print("default encryption_key value is used.\n");
  }

  // transmission_method_type
  switch (transmission_method_type_val) {
    case 0 :
    case 1 :
      config.transmission_method_type = transmission_method_type_val;

      break;
    default :
      err = 1;
      Print("transmission_method_type invalid value.\n");
      Print("default transmission_method_type value is used.\n");
  }

  // rssi_byte_flag
  switch (rssi_byte_flag_val) {
    case 0 :
    case 1 :
      config.rssi_byte_flag = rssi_byte_flag_val;

      break;
    default :
      err = 1;
      Print("rssi_byte_flag invalid value.\n");
      Print("default rssi_byte_flag value is used.\n");
  }
  // rssi_ambient_noise_flag
  switch (rssi_ambient_noise_flag_val) {
    case 0 :
    case 1 :
      config.rssi_ambient_noise_flag = rssi_ambient_noise_flag_val;

      break;
    default :
      err = 1;
      Print("rssi_ambient_noise_flag invalid value.\n");
      Print("default rssi_ambient_noise_flag value is used.\n");
  }

  // target_address
  if (ConfRange((int)target_address_val, 0, 65535)) {
    config.target_address = target_address_val;
  } else {
    err = 1;
    Print("target_address invalid value.\n");
    Print("default target_address value is used.\n");
  }

  // target_channel
  switch (bw_val) {
    case 125:
      if (ConfRange((int)target_channel_val, 0, 37)) {
        config.target_channel = target_channel_val;
      } else {
        err = 1;
        Print("target_channel invalid value.\n");
        Print("default target_channel value is used.\n");
      }
      break;
    case 250:
      if (ConfRange((int)target_channel_val, 0, 36)) {
        config.target_channel = target_channel_val;
      } else {
        err = 1;
        Print("target_channel_val invalid value.\n");
        Print("default target_channel_val value is used.\n");
      }
      break;
    case 500:
      if (ConfRange((int)target_channel_val, 0, 30)) {
        config.target_channel = target_channel_val;
      } else {
        err = 1;
        Print("target_channel_val invalid value.\n");
        Print("default target_channel_val value is used.\n");
      }
      break;
    default:
      err = 1;
      Print("bw invalid value.\n");
      Print("default target_channel_val value is used.\n");
      break;
  }

  return err;
}



#if ( (FIRMWARE_VERSION == 0x20) || (FIRMWARE_VERSION == 0x28) )



uint8_t CLoRa::ReadFirmwareVersion() {

  if ( Read_RegisterLIB(8, 1) ) return -1;
  return REGData.REGUniStr.val[0];
}

#else

const char ReadFirmWareVersionSTR[3] = {'\xc1', '\x08', '\x01'};  // ver.1.x compatible mode


uint8_t CLoRa::ReadFirmwareVersion() {
  uint8_t RetCode[5];

  SwitchToConfigurationMode();

  SerialLoRa.write(ReadFirmWareVersionSTR, 3);
  SerialLoRa.flush();
  delay(100);
  uint16_t len = 0;
  while (SerialLoRa.available()) {
    char ch = SerialLoRa.read();
    if (len > 5) continue;         // skip response (included if Received data in buffer exist)
    RetCode[len++] = ch;
  }
  Print("\n... \n");

  SwitchToNormalMode();

  return RetCode[3];
}
#endif




// -----------------------------------------------------------------------------------

#if ( (FIRMWARE_VERSION == 0x20) || (FIRMWARE_VERSION == 0x28) )


int CLoRa::init_REG09_0A_Byte() {
  //#if (FIRMWARE_VERSION < 0x20)
  //  return;
  //#endif

  int retcode = 0;

  REG09_Byte = SendPKT_CS_EN;        // for sending Initialize with CheckSUM
  REG0A_Byte = 0;                    // 0
  retcode = REG0A_ByteWrite();                 // Pseude Clear

  retcode |= REG09_ByteWrite(0b00000000);       // Pseude for sending Initialize with CheckSUM

  REG09_Byte = 0x00;
  retcode |= REG09_ByteWrite();                 // Genuine Clear 09H

  return retcode;
}

int CLoRa::SetConfigStrictMode() {
  int retcode = 0;
  // コンフィグモード(M0=1,M1=1)へ移行する
  SwitchToConfigurationMode();
  
  conf_REG09_Byte = config.Lowvoltage_operation << 7;
  conf_REG09_Byte |= config.strict_mode << 6;
  conf_REG09_Byte |= config.recv_target_address << 5;
  conf_REG09_Byte |= config.send_data_checksum << 4;
  conf_REG09_Byte |= config.recv_data_checksum << 3;
  conf_REG09_Byte |= config.recv_data_packetsize << 2;
  conf_REG09_Byte |= config.aux_low_holdtime;
  conf_REG0A_Byte = config.carriresense_timeout;
  
  Print("\nconf_REG09_Byte = %02x", conf_REG09_Byte);
  if ( !(conf_REG09_Byte & StrictMode_EN) ) return 1;

  retcode = REG09_ByteWrite(conf_REG09_Byte);
  REG09_Byte = conf_REG09_Byte;
  retcode |= REG0A_ByteWrite();

  // ノーマルモード(M0=0,M1=0)へ移行する
  SwitchToNormalMode();

  return retcode;
}






// --- the beginning of ReceiveFlame // ver.2.0

int  CLoRa::ReceiveFrame(struct RecvFrameE220900T22SJP_t *recv_frame) {
  int len = 0;
  uint8_t *start_p = recv_frame->recv_data;


  memset(recv_frame->recv_data, 0x00, sizeof(recv_frame->recv_data) / sizeof(recv_frame->recv_data[0]));

  uint8_t checksum = 0;
  uint8_t previous_checksum = 0;

  while (1) {
    while (SerialLoRa.available()) {
      if (xSemaphoreTake(xMutex, (portTickType)100) == pdTRUE) {
        uint8_t ch = SerialLoRa.read();
        if ( len < 32 ) Print("%02x ", ch);

        *(start_p + len) = ch;
        previous_checksum = checksum;
        checksum ^= ch;

        len++;
        xSemaphoreGive(xMutex);
      }
      if (len > 408) {
        return 1;
      }
    }

    if ((SerialLoRa.available() == 0) && (len > 0)) {
      delay(10);
      if (SerialLoRa.available() == 0) {
        recv_frame->recv_data_len = len;
        if ( config.rssi_byte_flag ) {
          recv_frame->recv_data_len -= 1;
          recv_frame->rssi = recv_frame->recv_data[len - 1] - 256;
        }
        break;
      }
    }
    delay(100);
  }

#if RECEIVE_DATA_CHECK == 0

  RcvPacket[0].packet_address = 0xffff;
  RcvPacket[0].recv_multi_pkt_f = 0;
  RcvPacket[0].packet_error = 0;
  RcvPacket[0].packet_rssi = recv_frame->rssi ;
  if ( config.rssi_byte_flag ) {
    recv_frame->recv_data_len++;
  }
  RcvPacket[0].packet_size = recv_frame->recv_data_len;


  RcvPacket[0].packet_data = &recv_frame->recv_data[0];


  if ( IsNow_RecvPKT_CS_EN) {
    RcvPacket[0].packet_rssi = recv_frame->rssi = recv_frame->recv_data[len - 2] - 256;

  }

  return 0;
}

#else


  //--- for treating Single-Packet or Multi-Packets

  int arrayNo = 0;
  int dataTop = 0;

  RcvPacket[0].packet_address = 0xffff;
  RcvPacket[0].recv_multi_pkt_f = 0;
  RcvPacket[0].packet_error = 0;
  RcvPacket[0].packet_rssi = -200;

  int recvdata_end_ptr = len - 1;

  if ( IsNow_RecvPKT_SIZE_EN ) {                      // REG9 setting Received Data Length

    RcvPacket[arrayNo].packet_data = &recv_frame->recv_data[1];
    RcvPacket[arrayNo].packet_size = len - 1;

    if ( recv_frame->recv_data[0] == recvdata_end_ptr  ) {

      // Single-Packet

      int rtcode = CheckAndGetPacket(recv_frame, recvdata_end_ptr, checksum, previous_checksum, arrayNo, dataTop);
      recv_frame->rssi = RcvPacket[0].packet_rssi;

      if (rtcode == -1) return rtcode;

    } else {

      // Multi-Packets

      int Next_dataTop = 0;
      int Whole_Received_len = len - 1;

      Print("\n\n MultiPacket!!! %d", len);
      if ( IsNow_RecvPKT_CS_EN)
        recv_frame->rssi = recv_frame->recv_data[len - 2] - 256;

      while (1) {
        len = recv_frame->recv_data[dataTop];                 // len is the size of 1-packet
        recvdata_end_ptr = dataTop + len;
        Next_dataTop += len + 1;                              // +1 is The Size data check するかしないか

        Print("\nlen = %d ", len);

        RcvPacket[arrayNo].packet_size = len;
        RcvPacket[arrayNo].recv_multi_pkt_f = 0;
        RcvPacket[arrayNo].packet_address = 0xffff;
        RcvPacket[arrayNo].recv_multi_pkt_f = 0;
        RcvPacket[arrayNo].packet_error = 0;
        RcvPacket[arrayNo].packet_rssi = -200;

        checksum = 0;

        // Checksum 計算
        for (int i = 0; i <= len  ; i++   ) {
          uint8_t dt = recv_frame->recv_data[dataTop + i];
          previous_checksum = checksum;
          checksum ^= dt;
        }
        RcvPacket[arrayNo].packet_data = &recv_frame->recv_data[dataTop + 1];

        int rtcode = CheckAndGetPacket(recv_frame, recvdata_end_ptr, checksum, previous_checksum, arrayNo, dataTop);
        if (arrayNo >= 16 ) break;

        dataTop = Next_dataTop;
        if ( Next_dataTop >= Whole_Received_len ) return rtcode;

        RcvPacket[arrayNo].recv_multi_pkt_f = 1;
        ++arrayNo;
      }
    }
  } else {
    RcvPacket[arrayNo].packet_data = &recv_frame->recv_data[0];
    RcvPacket[arrayNo].packet_size = len;
    RcvPacket[arrayNo].packet_rssi = recv_frame->rssi;

    Print("\n\n No Received Data Length!!!");
    int rtcode = CheckAndGetPacket(recv_frame, recvdata_end_ptr, checksum, previous_checksum, arrayNo, dataTop);
    if (rtcode == -1) return rtcode;

  }
  return 0;
}
//#endif

int CLoRa::CheckAndGetPacket(RECVstrc *recv_frame, int recvdata_end_ptr, uint8_t checksum, uint8_t previous_checksum, int arrayNo, int dataTop) {

  if ( IsNow_RecvPKT_CS_EN) {
    Print("\nRECEIVE CHECKSUM (%d)= %02x %02x %02x \n",
          arrayNo, checksum, recv_frame->recv_data[recvdata_end_ptr], previous_checksum);

    if (checksum != 0) {
      RcvPacket[arrayNo].packet_error = -1;
      return -1;
    }

    RcvPacket[arrayNo].packet_size -= 1;
    recvdata_end_ptr --;
  }

  if ( config.rssi_byte_flag ) {
    RcvPacket[arrayNo].packet_size -= 1 ;
    RcvPacket[arrayNo].packet_rssi = recv_frame->recv_data[recvdata_end_ptr] - 256;
  }
  //  if ( IsNow_RecvPKT_AD_EN ) {
  if ( (IsNow_RecvPKT_AD_EN) && (config.transmission_method_type == 1) ) {

    if ( IsNow_RecvPKT_SIZE_EN ) {
      RcvPacket[arrayNo].packet_address = recv_frame->recv_data[dataTop + 1] | (recv_frame->recv_data[dataTop + 2] << 8);
    } else {
      RcvPacket[arrayNo].packet_address = recv_frame->recv_data[dataTop] | (recv_frame->recv_data[dataTop + 1] << 8);
    }
    RcvPacket[arrayNo].packet_size -= 2;
    RcvPacket[arrayNo].packet_data += 2;
  } else {
    RcvPacket[arrayNo].packet_address = config.own_address;
  }
  return 0;
}

#endif


int CLoRa::Check_BufferedReceiveData_Exist() {
  Buffered_ReceiveData = 0;

  if ( !SerialLoRa.available() ) return 1;
  if ( !ReceiveFrame(&data) ) {
    Print("\n No Received data in buffer");
    Buffered_ReceiveData = 1;
  }
  else  return 1;

  Print("\n\nReceived Data Exist.");
  for (int j = 0;; j++) {

    Print("\n RcvPacket[%d].packet_size = %d", j, RcvPacket[j].packet_size);
    Print("\n RcvPacket[%d].packet_address %02x%02x", j, RcvPacket[j].packet_address >> 8 , RcvPacket[j].packet_address & 0xff);
    Print("\n RcvPacket[%d].packet_data = ", j);
    for (int i = 0; i < RcvPacket[j].packet_size; i++) {
      Print(" %02x", RcvPacket[j].packet_data[i]);
      if ( i > 32 ) break;
    }
    Print("\n RcvPacket[%d].recv_multi_pkt_f = %d", j, RcvPacket[j].recv_multi_pkt_f);
    Print("\n RcvPacket[%d].packet_rssi = %d", j, RcvPacket[j].packet_rssi);

    if (RcvPacket[j].recv_multi_pkt_f == 0) break;
  }
  return 0;
}


// --- the end of ReceiveFlame // ver.2.0, ver2.8
#endif


#if ( (FIRMWARE_VERSION == 0x20) || (FIRMWARE_VERSION == 0x28))

int CLoRa::Read_RegisterLIB( uint8_t address, uint8_t bytes) {

  char checksum = 0;

  Check_BufferedReceiveData_Exist();

  REGData.REGUniStr.error = 0;

  SwitchToConfigurationMode();

  uint8_t ch = 0xc1;
  SerialLoRa.write(ch);
  checksum ^= ch;
  SerialLoRa.write(address);
  checksum ^= address;
  SerialLoRa.write(bytes);
  checksum ^= bytes;
  Print("\n++++ Read Register %02x %02x %02x ", ch, address, bytes);

  if ( IsNow_SendPKT_CS_EN ) {
    SerialLoRa.write(checksum);
    Print("%02x ", checksum);
  }

  SerialLoRa.flush();
  delay(100);

  Print("\n");

  uint16_t len = 0;
  while (SerialLoRa.available()) {
    uint8_t ch = SerialLoRa.read();
    Print("0x%02x ", ch);
    REGData.data[len++]  = ch;
  }
  Print("\n");
  if (len == 0) {
    Print("\n... No Response ...\n");
    REGData.REGUniStr.error = -1;

    return 1;
  }
  else {
    for (int i = 0; i < bytes + 3 ; i++) Print("%02x ", REGData.data[i] );
  }

  SwitchToNormalMode();

  return 0;
}

int CLoRa::Read_RegisterLIB_s( uint8_t address, uint8_t bytes) {

  char checksum = 0;
  int recvflag = 0;
  int retcode = 0;

  Check_BufferedReceiveData_Exist();


  REGData.REGUniStr.error = 0;


  uint8_t ch = 0xc1;
  SerialLoRa.write(ch);
  checksum ^= ch;
  SerialLoRa.write(address);
  checksum ^= address;
  SerialLoRa.write(bytes);
  checksum ^= bytes;
  Print("\n++++ Read Register %02x %02x %02x ", ch, address, bytes);

  if ( IsNow_SendPKT_CS_EN ) {
    SerialLoRa.write(checksum);
    Print("%02x ", checksum);
  }

  SerialLoRa.flush();
  delay(200);

  Print("\n");

  uint16_t len = 0;
  while (SerialLoRa.available()) {
    uint8_t ch = SerialLoRa.read();
    Print("0x%02x ", ch);
    REGData.data[len++]  = ch;
  }
  Print("\n");
  if (len == 0) {
    Print("... No Response ...\n");
    REGData.REGUniStr.error = 1;
    retcode = 1;
  }
  else {
    for (int i = 0; i < bytes + 3 ; i++) Print("%02x ", REGData.data[i] );
  }

  return retcode;
}


int CLoRa::Write_RegisterLIB(const char* str, int str_length, uint8_t SetData) {

  char checksum = 0;

  Check_BufferedReceiveData_Exist();

  SwitchToConfigurationMode();
  PrintLn('\n');

  SerialLoRa.write(str, str_length);


  for (int i = 0; i < str_length ; i++) {
    char ch = str[i];
    checksum ^= ch;
    Print("%02x ", ch);
  }
  SerialLoRa.write(SetData);
  Print("%02x ", SetData);

  checksum ^= SetData;

  if ( IsNow_SendPKT_CS_EN ) {
    SerialLoRa.write(checksum);
    Print("%02x ", checksum);

  }
  SerialLoRa.flush();
  delay(100);

  Print("\n");

  return Skip_Receive_LoRa_Response();

}

int CLoRa::Skip_Receive_LoRa_Response() {
  int retcode = 0;

  uint16_t len = 0;
  while (SerialLoRa.available()) {
    char ch = SerialLoRa.read();
    Print("%02x ", ch);
    len++;
  }

  if (len == 0) {
    Print("... No Response ...\n");
    retcode = 1;
  }

  SwitchToNormalMode();

  return retcode;
}


//---
const char CNF_Write_09[3] = {'\xc0', '\x09', '\x01'};
const char CNF_Write_0A[3] = {'\xc0', '\x0A', '\x01'};


int CLoRa::REG09_ByteWrite() {
  int retcode = Write_RegisterLIB(CNF_Write_09, 3, REG09_Byte);
  PrintLn('\n');
  return retcode;
}

int CLoRa::REG09_ByteWrite(uint8_t setdata) {
  int retcode = Write_RegisterLIB(CNF_Write_09, 3, setdata);
  PrintLn('\n');
  return retcode;
}

int CLoRa::REG0A_ByteWrite() {
  int retcode = Write_RegisterLIB(CNF_Write_0A, 3, REG0A_Byte);
  PrintLn('\n');
  return retcode;
}

//---

int CLoRa::REG09_Set_AUX_HoldTime(uint8_t bt) {  /**  write AUX Hold Low Time */
  REG09_Byte = (REG09_Byte & 0b11111100) | bt;
  int retcode = REG09_ByteWrite();
  PrintLn('\n');
  return retcode;
}

int CLoRa::REG0A_Set_CarrierSenseTime(uint8_t ms) {   /** write AUX Hold Low Time */
  REG0A_Byte = ms;
  int retcode = REG0A_ByteWrite();
  PrintLn('\n');
  return retcode;
}
int CLoRa::Lowvoltage_operation_Enable() {
  REG09_Byte &= 0b01111111;
  int retcode = REG09_ByteWrite();
  return retcode;
}

int CLoRa::Lowvoltage_operation_Disable() {
  REG09_Byte |= 0b10000000;
  int retcode = REG09_ByteWrite();
  return retcode;
}


int CLoRa::StrictModeStart() {
  REG09_Byte |= StrictMode_EN;
  int retcode = REG09_ByteWrite();
  return retcode;

}

int CLoRa::StrictModeEnd() {
  REG09_Byte &= (StrictMode_EN ^ 0b11111111);
  int retcode = REG09_ByteWrite();
  return retcode;
}


int CLoRa::ReceivePKTaddCS() {
  if ( !IsNow_StrictMode ) return -1;

  REG09_Byte |= RecvPKT_CS_EN;
  int retcode = REG09_ByteWrite();
  return retcode;
}

int CLoRa::ReceivePKTdelCS() {
  if ( !IsNow_StrictMode ) return -1;

  REG09_Byte &= RecvPKT_CS_EN ^ 0b11111111;
  int retcode = REG09_ByteWrite();
  return retcode;
}

int CLoRa::SendPKTaddCS() {
  if ( !IsNow_StrictMode ) return -1;

  uint8_t setdata;
  setdata = REG09_Byte | SendPKT_CS_EN;
  int retcode = REG09_ByteWrite(setdata);
  REG09_Byte = setdata;
  return retcode;
}

int CLoRa::SendPKTdelCS() {
  if ( !IsNow_StrictMode ) return -1;

  uint8_t setdata;
  setdata = REG09_Byte & (SendPKT_CS_EN ^ 0b11111111);
  int retcode = REG09_ByteWrite(setdata);
  REG09_Byte = setdata;
  return retcode;
}

int CLoRa::ReceivePKTaddADRS() {
  if ( !IsNow_StrictMode ) return -1;

  REG09_Byte |= RecvPKT_AD_EN;
  int retcode = REG09_ByteWrite();
  return retcode;
}

int CLoRa::ReceivePKTdelADRS() {
  if ( !IsNow_StrictMode ) return -1;

  REG09_Byte &= RecvPKT_AD_EN ^ 0b11111111;
  int retcode = REG09_ByteWrite();
  return retcode;
}

int CLoRa::ReceivePKTaddSIZE() {
  if ( !IsNow_StrictMode ) return -1;

  REG09_Byte |= RecvPKT_SIZE_EN;
  int retcode = REG09_ByteWrite();
  return retcode;
}


int CLoRa::ReceivePKTdelSIZE() {
  if ( !IsNow_StrictMode ) return -1;

  REG09_Byte &= RecvPKT_SIZE_EN ^ 0b11111111;
  int retcode = REG09_ByteWrite();
  return retcode;
}



uint8_t CLoRa::write_id_num(char RegNum, uint8_t IDdata) {

  uint8_t retcode = 0;

  Check_BufferedReceiveData_Exist();

  SwitchToConfigurationMode();

  char checksum = 0;

  char WriteCommandChar = 0xc0;
  Print("\nWrite ID num Register =  ");

  SerialLoRa.write(WriteCommandChar);
  Print("%02x ", WriteCommandChar);

  SerialLoRa.write(RegNum);
  Print("%02x ", RegNum);
  SerialLoRa.write('\01');
  Print("%02x ", '\01');
  SerialLoRa.write(IDdata);
  Print("%02x ", IDdata);

  checksum ^= WriteCommandChar;
  checksum ^= RegNum;
  checksum ^= 0x01;
  checksum ^= IDdata;

  if ( IsNow_SendPKT_CS_EN ) {
    SerialLoRa.write(checksum);
    Print("%02x ", checksum);
  }

  SerialLoRa.flush();
  delay(100);

  Print("\n");

  return Skip_Receive_LoRa_Response();

}

uint8_t CLoRa::read_id_num(char RegNum) {
  uint8_t retcode = 0;

  Check_BufferedReceiveData_Exist();

  SwitchToConfigurationMode();

  char checksum = 0;

  Print("\nRead ID num Register =  ");

  char WriteCommandChar = 0xc1;
  SerialLoRa.write(WriteCommandChar);
  Print("%02x ", WriteCommandChar);
  SerialLoRa.write(RegNum);
  Print("%02x ", RegNum);
  SerialLoRa.write('\x01');
  Print("%02x ", '\x01');

  checksum ^= WriteCommandChar;
  checksum ^= RegNum;
  checksum ^= 0x01;

  if ( IsNow_SendPKT_CS_EN ) {
    SerialLoRa.write(checksum);
    Print("%02x ", checksum);
  }


  SerialLoRa.flush();
  delay(100);

  Print("\n");

  uint16_t len = 0;
  while (SerialLoRa.available()) {
    char ch = SerialLoRa.read();
    Print("0x%02x ", ch);
    REGData.data[len++]  = ch;
  }
  if (len == 0) {
    Print("... No Response ...\n");
    uint8_t retcode = 1;
  }
  SwitchToNormalMode();
  if ( IsNow_RecvPKT_SIZE_EN ) {
    for ( int i = 0; i < len ; i++) REGData.data[i] = REGData.data[i + 1];
  }
  return 0;
}




#endif
