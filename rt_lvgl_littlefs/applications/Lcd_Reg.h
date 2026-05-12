#ifndef __LCD_REG_H
#define __LCD_REG_H

/*
 * ILI9341 常用命令寄存器定义。
 * 这里只保留当前 LCD 初始化、设置窗口、写像素真正用到的命令，
 * 方便你先理解“点亮屏幕”和“LVGL 刷屏”这条主链路。
 */
#define ILI9341_NOP             0x00
#define ILI9341_SWRESET         0x01
#define ILI9341_RDDID           0x04
#define ILI9341_RDDST           0x09

#define ILI9341_SLPIN           0x10
#define ILI9341_SLPOUT          0x11
#define ILI9341_PTLON           0x12
#define ILI9341_NORON           0x13

#define ILI9341_INVOFF          0x20
#define ILI9341_INVON           0x21
#define ILI9341_GAMSET          0x26
#define ILI9341_DISPOFF         0x28
#define ILI9341_DISPON          0x29
#define ILI9341_CASET           0x2A
#define ILI9341_PASET           0x2B
#define ILI9341_RAMWR           0x2C
#define ILI9341_RAMRD           0x2E

#define ILI9341_MADCTL          0x36
#define ILI9341_PIXFMT          0x3A
#define ILI9341_POWERA          0xCB
#define ILI9341_POWERB          0xCF
#define ILI9341_DTCA            0xE8
#define ILI9341_DTCB            0xEA
#define ILI9341_POWER_SEQ       0xED
#define ILI9341_FRMCTR1         0xB1
#define ILI9341_DFUNCTR         0xB6
#define ILI9341_ENABLE3G        0xF2
#define ILI9341_PRC             0xF7
#define ILI9341_PWCTR1          0xC0
#define ILI9341_PWCTR2          0xC1
#define ILI9341_VMCTR1          0xC5
#define ILI9341_VMCTR2          0xC7
#define ILI9341_GMCTRP1         0xE0
#define ILI9341_GMCTRN1         0xE1

/*
 * MADCTL(0x36) 控制显示方向和颜色顺序。
 *
 * 常见现象和这些位的关系：
 * - 画面上下/左右翻转：通常看 MY/MX。
 * - 横屏竖屏切换：通常看 MV。
 * - 红蓝颜色反了：通常看 BGR。
 */
#define ILI9341_MADCTL_MY       0x80
#define ILI9341_MADCTL_MX       0x40
#define ILI9341_MADCTL_MV       0x20
#define ILI9341_MADCTL_ML       0x10
#define ILI9341_MADCTL_BGR      0x08
#define ILI9341_MADCTL_MH       0x04

#endif /* __LCD_REG_H */
