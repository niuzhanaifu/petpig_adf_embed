/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "sdkconfig.h"

/* Example configurations */
#define USER_SAMPLE_RATE     (16000) //16000
#define USER_MCLK_MULTIPLE   (384) // If not using 24-bit data width, 256 should be enough
#define USER_MCLK_FREQ_HZ    (USER_SAMPLE_RATE * USER_MCLK_MULTIPLE)
#define USER_VOICE_VOLUME    75
#if CONFIG_EXAMPLE_MODE_ECHO
#define EXAMPLE_MIC_GAIN        CONFIG_EXAMPLE_MIC_GAIN
#endif


#if !defined(CONFIG_EXAMPLE_BSP)

#define NS4150B_CTRL    (GPIO_NUM_10)

/* I2C port and GPIOs */
#define I2C_NUM         (0)
#define I2C_SCL_IO      (GPIO_NUM_4)
#define I2C_SDA_IO      (GPIO_NUM_5)

/* I2S port and GPIOs */
#define I2S_NUM         (0)
#define I2S_MCK_IO      (GPIO_NUM_6)
#define I2S_BCK_IO      (GPIO_NUM_14)
#define I2S_WS_IO       (GPIO_NUM_12)

#define I2S_DO_IO       (GPIO_NUM_11)
#define I2S_DI_IO       (GPIO_NUM_13)


#else // CONFIG_EXAMPLE_BSP
#include "bsp/esp-bsp.h"
#define I2C_NUM BSP_I2C_NUM

#endif // CONFIG_EXAMPLE_BSP
