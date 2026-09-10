#pragma once

/*
 * HUB75 hardware-specific pinout collection for ESP32 targets
 *
 * Copyright (c) 2020 Louis Beaudoin (Pixelmatix)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "globals.h"

 // Note: only one MatrixHardware_*.h file should be included per project

#ifndef MATRIX_HARDWARE_H
#define MATRIX_HARDWARE_H
    /*
    HUB 75
    01 02 B0
    03 04 Gnd
    05 06 G1
    07 08 E

    09 10 B
    11 12 D
    13 14 STB/Latch
    15 16 Gnd
                        ESP32 pin / comment
    1   R0  2   Red Data (columns 1-16)
    2   G0  15  Green Data (columns 1-16)

    3   B0  4   Blue Data (columns 1-16)
    4   GND GND Ground

    5   R1  16/RX2  Red Data (columns 17-32)
    6   G1  27  Green Data (columns 17-32)

    7   B1  17/TX2  Blue Data (columns 17-32)
    8   E   12  Demux Input E for 64x64 panels

    9   A   5   Demux Input A0
    10  B   18  Demux Input A1

    11  C   19  Demux Input A2
    12  D   21  Demux Input E1, E3 (32x32 panels only)

    13  CLK 22  LED Drivers' Clock
    14  STB 26  LED Drivers' Latch

    15  OE  25  LED Drivers' Output Enable
    16  GND GND Ground
    */

    /* Mesmerizer Classic Breadboard Setup */
    /*
    #define R1_PIN  GPIO_NUM_12
    #define G1_PIN  GPIO_NUM_13
    #define B1_PIN  GPIO_NUM_26
    #define R2_PIN  GPIO_NUM_22
    #define G2_PIN  GPIO_NUM_27
    #define B2_PIN  GPIO_NUM_19
    #define A_PIN   GPIO_NUM_5
    #define B_PIN   GPIO_NUM_23
    #define C_PIN   GPIO_NUM_0
    #define D_PIN   GPIO_NUM_21
    #define E_PIN   GPIO_NUM_25
    #define LAT_PIN GPIO_NUM_18
    #define OE_PIN  GPIO_NUM_4
    #define CLK_PIN GPIO_NUM_2
    */

    #if defined(WAVESHARE_ESP32S3_RGB_MATRIX) && WAVESHARE_ESP32S3_RGB_MATRIX
        // Waveshare ESP32-S3-RGB-Matrix driver board's documented default pinout
        // (github.com/waveshareteam/ESP32-S3-RGB-Matrix example sketches).
        #define R1_PIN  GPIO_NUM_4
        #define G1_PIN  GPIO_NUM_5
        #define B1_PIN  GPIO_NUM_6
        #define R2_PIN  GPIO_NUM_7
        #define G2_PIN  GPIO_NUM_15
        #define B2_PIN  GPIO_NUM_16
        #define A_PIN   GPIO_NUM_18
        #define B_PIN   GPIO_NUM_8
        #define C_PIN   GPIO_NUM_3
        #define D_PIN   GPIO_NUM_42
        #define E_PIN   GPIO_NUM_9
        #define LAT_PIN GPIO_NUM_40
        #define OE_PIN  GPIO_NUM_2
        #define CLK_PIN GPIO_NUM_41
    #elif defined(MESMERIZER_DEVKIT_S3) && MESMERIZER_DEVKIT_S3
        // ESP32-S3-DevKitC-1 HUB75 wiring.
        #define R1_PIN  GPIO_NUM_18
        #define G1_PIN  GPIO_NUM_8
        #define B1_PIN  GPIO_NUM_17
        #define R2_PIN  GPIO_NUM_16
        #define G2_PIN  GPIO_NUM_1
        #define B2_PIN  GPIO_NUM_15
        #define A_PIN   GPIO_NUM_7
        #define B_PIN   GPIO_NUM_48
        #define C_PIN   GPIO_NUM_6
        #define D_PIN   GPIO_NUM_47
        #define E_PIN   GPIO_NUM_2
        #define LAT_PIN GPIO_NUM_21
        #define OE_PIN  GPIO_NUM_4
        #define CLK_PIN GPIO_NUM_5
    #elif defined(MESMERIZER_DEVKIT) && MESMERIZER_DEVKIT
        // ESP32-DevKitC V4 HUB75 wiring.
        #define R1_PIN  GPIO_NUM_18
        #define G1_PIN  GPIO_NUM_17
        #define B1_PIN  GPIO_NUM_19
        #define R2_PIN  GPIO_NUM_21
        #define G2_PIN  GPIO_NUM_23
        #define B2_PIN  GPIO_NUM_27
        #define A_PIN   GPIO_NUM_26
        #define B_PIN   GPIO_NUM_16
        #define C_PIN   GPIO_NUM_25
        #define D_PIN   GPIO_NUM_4
        #define E_PIN   GPIO_NUM_22
        #define LAT_PIN GPIO_NUM_2
        #define OE_PIN  GPIO_NUM_32
        #define CLK_PIN GPIO_NUM_33
    #else
        #define R1_PIN  GPIO_NUM_2
        #define G1_PIN  GPIO_NUM_0
        #define B1_PIN  GPIO_NUM_32
        #define R2_PIN  GPIO_NUM_25
        #define G2_PIN  GPIO_NUM_33
        #define B2_PIN  GPIO_NUM_27
        #define A_PIN   GPIO_NUM_5
        #define B_PIN   GPIO_NUM_4
        #define C_PIN   GPIO_NUM_19
        #define D_PIN   GPIO_NUM_18
        #define E_PIN   GPIO_NUM_26
        #define LAT_PIN GPIO_NUM_21
        #define OE_PIN  GPIO_NUM_23
        #define CLK_PIN GPIO_NUM_22
    #endif

//#define DEBUG_PINS_ENABLED
//#define DEBUG_1_GPIO    GPIO_NUM_13
//#define DEBUG_2_GPIO    GPIO_NUM_12

#else
    #pragma GCC error "Multiple MatrixHardware*.h files included"
#endif
