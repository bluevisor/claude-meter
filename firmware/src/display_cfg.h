#pragma once

#include <Arduino_GFX_Library.h>
#include <SensorQMI8658.hpp>
#include <Wire.h>

// Waveshare ESP32-S3-LCD-1.3: 1.3" 240x240 ST7789V2 LCD via SPI, mounted
// under a prism cube. QMI8658 IMU on I2C. No touch, no PMU, no battery
// gauge. Backlight is hard-wired (not GPIO-controlled). Pin assignments
// confirmed against Waveshare's official Arduino demo (ESP32-S3-LCD-1.3-Demo
// → libraries/TFT_eSPI/User_Setup.h and examples/03_SDIMG_Game2048/WS_QMI8658.cpp).

#define LCD_WIDTH   240
#define LCD_HEIGHT  240

// SPI display pins (ST7789V2)
#define LCD_CS       39
#define LCD_DC       38
#define LCD_RST      42
#define LCD_MOSI     41
#define LCD_SCLK     40

// I2C bus shared with QMI8658 6-axis IMU
#define IIC_SDA      47
#define IIC_SCL      48

// Single user button — BOOT (GPIO 0). The board exposes no other tactile
// inputs; BOOT cycles screens.
#define BTN_USER     0

extern Arduino_DataBus *bus;
extern Arduino_ST7789  *gfx;
extern SensorQMI8658    imu;
