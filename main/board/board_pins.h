/*
 * ONX3248G035 (schematic V1.2, 2025-07-21) pin map.
 *
 *  ESP32-S3R8, 16 MB flash (PY25Q128), 8 MB octal PSRAM
 *  LCD  : ST7796 320x480, 4-wire SPI   MOSI=IO1  SCLK=IO5  CS=IO2  DC=IO3
 *         RST via PCF8574 P5, backlight IO6 (NPN Q5, active high, PWM)
 *  Touch: CST826 @0x15 on I2C,  INT via PCF8574 P4, RST shared with LCD_RST
 *  I2C  : SCL=IO7 SDA=IO8  (PCF8574A @0x38, CST826 @0x15, BLX8563 RTC @0x51)
 *  SD   : SDMMC 1-bit  CLK=IO11 CMD=IO10 D0=IO9, CS/DAT3 via PCF8574 P6
 *  BAT  : IO4 (ADC1_CH3) through 1M / 2.2M divider  -> Vbat = Vadc * 3.2 / 2.2
 *
 *  PCF8574A port: P0 I2S_CTRL, P1 RTC_INT, P2 CHG_N, P3 CAM_PWDN,
 *                 P4 TP_INT,   P5 LCD_RST, P6 SDCS,  P7 nc
 */
#pragma once

#include "driver/gpio.h"

#define BOARD_I2C_PORT          0
#define BOARD_I2C_SCL           GPIO_NUM_7
#define BOARD_I2C_SDA           GPIO_NUM_8

#define BOARD_LCD_SPI_HOST      SPI2_HOST
#define BOARD_LCD_MOSI          GPIO_NUM_1
#define BOARD_LCD_SCLK          GPIO_NUM_5
#define BOARD_LCD_CS            GPIO_NUM_2
#define BOARD_LCD_DC            GPIO_NUM_3
#define BOARD_LCD_BL            GPIO_NUM_6
#define BOARD_LCD_H_RES         320
#define BOARD_LCD_V_RES         480
#define BOARD_LCD_PCLK_HZ       (40 * 1000 * 1000)

#define BOARD_SD_CLK            GPIO_NUM_11
#define BOARD_SD_CMD            GPIO_NUM_10
#define BOARD_SD_D0             GPIO_NUM_9

#define BOARD_BAT_ADC_GPIO      GPIO_NUM_4

#define BOARD_PCF8574_ADDR      0x38
#define BOARD_CST826_ADDR       0x15

/* PCF8574 bit numbers */
#define EXIO_I2S_CTRL           0
#define EXIO_RTC_INT            1
#define EXIO_CHG_N              2
#define EXIO_CAM_PWDN           3
#define EXIO_TP_INT             4
#define EXIO_LCD_RST            5
#define EXIO_SD_CS              6

#define SD_MOUNT_POINT          "/sdcard"
#define SPIFFS_MOUNT_POINT      "/spiffs"
