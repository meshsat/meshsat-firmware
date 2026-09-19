#include "../tbeam-s3-core/variant.h"

// RockBLOCK 9603 on header PM1: pin 13 U0TXD to modem pin 6, pin 12 U0RXD from modem pin 1.
// UART1 belongs to the GPS, so the modem gets UART2.
#define MESHSAT_IRIDIUM_TX_PIN 43
#define MESHSAT_IRIDIUM_RX_PIN 44
#define MESHSAT_IRIDIUM_UART_NUM 2
#define MESHSAT_IRIDIUM_BAUD 19200

// Modem supply from the AXP2101 DCDC5 rail (PM1 pin 9, 1.4-3.7 V, 1 A); the RockBLOCK takes 3.0-5.4 V.
#define MESHSAT_IRIDIUM_DCDC5_MV 3700
