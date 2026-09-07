#ifndef __LCD_H
#define __LCD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <board.h>

/*
 * 本工程使用的 ILI9341 屏幕分辨率。
 * 当前方向是竖屏：240 x 320。
 */
#define LCD_WIDTH               240U
#define LCD_HEIGHT              320U

/*
 * LCD 控制引脚。
 * PB12 -> LCD_CS
 * PC5  -> LCD_DC
 * PB1  -> LCD_BLK
 */
#define LCD_CS_Pin              GPIO_PIN_12
#define LCD_CS_GPIO_Port        GPIOB
#define LCD_DC_Pin              GPIO_PIN_5
#define LCD_DC_GPIO_Port        GPIOC
#define LCD_BLK_Pin             GPIO_PIN_1
#define LCD_BLK_GPIO_Port       GPIOB

void LCD_Init(void);

/*
 * 设置 LCD 内部的写入窗口。
 * 后续写入的像素会按顺序填进这个矩形区域。
 */
void LCD_Set_Window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);

/* 下面三个接口是 ILI9341 寄存器写入的基础积木。 */
void LCD_WriteCommand(uint8_t cmd);
void LCD_WriteData8(uint8_t data);
void LCD_WriteData16(uint16_t data);

/*
 * 阻塞式像素写入接口。
 * LVGL 刷屏的典型顺序：
 * 1. LCD_Set_Window(...)
 * 2. LCD_WritePixels(...)
 * 3. lv_disp_flush_ready(...)
 */
HAL_StatusTypeDef LCD_WritePixels(const uint8_t *data, uint32_t size);

/* 用同一种 RGB565 颜色填充当前窗口，常用于屏幕点亮测试。 */
void LCD_Fill_Color(uint16_t color);

#ifdef __cplusplus
}
#endif

#endif /* __LCD_H */
