// MeshSat node: XIAO ESP32-S3 + Wio-SX1262 (B2B) + RockBLOCK 9603 (Iridium SBD).
// Same pins as seeed_xiao_s3, except GPIO43/44 carry the RockBLOCK instead of the L76K GPS.

// Wio-SX1262 green LED, lit when high.
#define LED_POWER 48
#define LED_STATE_ON 1

// Wio-SX1262 user button (10 k pull-up, pressed = low). The XIAO user LED
// shares GPIO21.
#define BUTTON_PIN 21
#define BUTTON_NEED_PULLUP

#define BATTERY_PIN -1
#define ADC_CHANNEL ADC_CHANNEL_0
#define BATTERY_SENSE_RESOLUTION_BITS 12

// Optional 1.3 inch OLED on the XIAO expansion board.
#define USCREEN_SSD1306
#define I2C_SDA 5
#define I2C_SCL 6

// Wio-SX1262 on the B2B connector.
#define USE_SX1262

#define LORA_MISO 8
#define LORA_SCK 7
#define LORA_MOSI 9
#define LORA_CS 41

#define LORA_RESET 42
#define LORA_DIO1 39

#define LORA_DIO2 38

#ifdef USE_SX1262
#define SX126X_CS LORA_CS
#define SX126X_DIO1 LORA_DIO1
#define SX126X_BUSY 40
#define SX126X_RESET LORA_RESET

// DIO2 controls an antenna switch and DIO3 sets the TCXO voltage.
#define SX126X_DIO2_AS_RF_SWITCH
#define SX126X_RXEN 38
#define SX126X_TXEN RADIOLIB_NC
#define SX126X_DIO3_TCXO_VOLTAGE 1.8
#endif

// RockBLOCK on the XIAO's own D6/D7 pads; the Wio-SX1262's D5/D6/D7 pads are no-connect.
// D6 = GPIO43 -> RockBLOCK pin 6 (TXD, modem input); D7 = GPIO44 <- RockBLOCK pin 1 (RXD, modem output).
#define MESHSAT_IRIDIUM_TX_PIN 43
#define MESHSAT_IRIDIUM_RX_PIN 44
// UART1 is free because GPS is compiled out. Ground Control: 19200 8N1, no flow control.
#define MESHSAT_IRIDIUM_UART_NUM 1
#define MESHSAT_IRIDIUM_BAUD 19200
