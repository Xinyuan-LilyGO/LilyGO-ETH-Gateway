/*
 / _____)             _              | |
( (____  _____ ____ _| |_ _____  ____| |__
 \____ \| ___ |    (_   _) ___ |/ ___)  _ \
 _____) ) ____| | | || |_| ____( (___| | | |
(______/|_____)_|_|_| \__)_____)\____)_| |_|
  (C)2019 Semtech

Description:
    Functions to reset LoRa concentrator from GPIO Pins.

License: Revised BSD License, see LICENSE.TXT file include in the project
*/

#ifndef _LORAGW_GPIO_H
#define _LORAGW_GPIO_H


#include "driver/gpio.h"

#ifndef SX1302_RESET_PIN
#define SX1302_RESET_PIN          2
#endif

#ifndef SX1302_POWER_EN_PIN
#define SX1302_POWER_EN_PIN       4
#endif

#define SX1302_GPIO_PIN_SEL       (1ULL << SX1302_RESET_PIN)

// reset the gateway using RESET and POWER_EN GPIO Pins.
void lgw_reset(void);

#endif
