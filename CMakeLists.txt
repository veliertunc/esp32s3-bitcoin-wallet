# ESP32-S3 Bitcoin hardware wallet for the ONX3248G035 board
# (ESP32-S3R8, ST7796 320x480 SPI LCD, CST826 touch, PCF8574A IO expander, microSD)
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32s3-bitcoin-wallet)
