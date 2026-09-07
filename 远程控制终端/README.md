# RT-Thread + LVGL + LittleFS 嵌入式监控项目

## 概述

基于 **RT-Thread** 实时操作系统，集成 **LVGL** 图形用户界面库和 **LittleFS** 文件系统的嵌入式监控仪表盘项目。

- **MCU**: ARM Cortex-M4 (STM32)
- **显示**: ILI9341 LCD, 240×320 像素, SPI 接口
- **GUI**: LVGL，显示系统运行监控仪表盘

## 功能特性

- **RT-Thread 内核**: 多线程、信号量、互斥锁、事件、邮箱、消息队列、内存管理、设备框架
- **LVGL 图形界面**: 实时显示系统运行信息（运行时间、构建日期、堆使用率、当前线程、系统心跳）
- **LittleFS 文件系统**: 基于 SPI Flash 的轻量级掉电安全文件系统
- **Finsh/MSH 控制台**: 通过 UART 进行命令行交互调试
- **SPI 驱动 / SFUD**: 支持 SPI Flash 通用驱动库
- **设备文件系统 (DFS)**: 支持 devfs 设备文件系统

## 界面说明

主界面 "RT-Thread Monitor" 包含以下区域：

| 区域 | 内容 |
|------|------|
| 顶部面板 | 运行时间、构建日期、心跳指示灯 |
| 中间面板 | 当前线程/优先级、堆使用量、系统滴答 |
| 底部面板 | LCD 设备状态 |
| 色块装饰 | 6 色装饰条 |

## 硬件依赖

- STM32 开发板 (Cortex-M4)
- ILI9341 SPI LCD (240×320)
- SPI NOR Flash (用于 LittleFS)

## 构建

使用 RT-Thread Studio 或 scons 命令行工具构建：

```bash
scons
```

## 目录结构

```
├── applications/        # 用户应用代码
│   ├── main.c           # 主程序入口，LVGL 仪表盘
│   ├── Lcd.c / Lcd.h   # ILI9341 LCD 驱动
│   └── lv_port_disp.*   # LVGL 显示移植接口
├── cubemx/              # STM32CubeMX 配置
├── drivers/             # 板级驱动
├── libraries/           # 芯片 HAL 库
├── rt-thread/           # RT-Thread 内核源码
├── lvgl/                # LVGL 图形库
├── packages/            # 软件包 (含 LittleFS v2.5.0)
├── linkscripts/         # 链接脚本
├── rtconfig.h           # RT-Thread 配置
└── SConstruct           # SCons 构建脚本
```
