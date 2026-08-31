#ifndef USER_CONFIG_H
#define USER_CONFIG_H

// Waveshare ESP32-S3-Touch-AMOLED-1.75C pin map.
// Source of truth: XiaoZhi board definition
// ref-ESP32-S3-Touch-AMOLED-1.75C/02_Example/XiaoZhi-v2.4.2/main/boards/
// waveshare/esp32-s3-touch-amoled-1.75/config.h (1_75C branch).

#define BSP_I2C_NUM             (I2C_NUM_0)

#define BSP_I2C_SCL             (GPIO_NUM_14)
#define BSP_I2C_SDA             (GPIO_NUM_15)

#define BSP_I2S_MCLK            (GPIO_NUM_16)
#define BSP_I2S_SCLK            (GPIO_NUM_9)
#define BSP_I2S_LCLK            (GPIO_NUM_45)
#define BSP_I2S_DOUT            (GPIO_NUM_8)
#define BSP_I2S_DSIN            (GPIO_NUM_10)
#define BSP_POWER_AMP_IO        (GPIO_NUM_46)

#define BSP_LCD_H_RES           (466)
#define BSP_LCD_V_RES           (466)
#define BSP_LCD_CS              (GPIO_NUM_12)
#define BSP_LCD_PCLK            (GPIO_NUM_38)
#define BSP_LCD_DATA0           (GPIO_NUM_4)
#define BSP_LCD_DATA1           (GPIO_NUM_5)
#define BSP_LCD_DATA2           (GPIO_NUM_6)
#define BSP_LCD_DATA3           (GPIO_NUM_7)
#define BSP_LCD_RST             (GPIO_NUM_1)

#define BSP_LCD_DMASIZE         (BSP_LCD_H_RES * BSP_LCD_V_RES)

#define BSP_LCD_BACKLIGHT       (GPIO_NUM_NC)
#define BSP_LCD_TOUCH_RST       (GPIO_NUM_2)
#define BSP_LCD_TOUCH_INT       (GPIO_NUM_11)

#endif // !USER_CONFIG_H
