/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-04-26     RT-Thread    first version
 */

#include <rtthread.h>
#include <rtdbg.h>

#include "Lcd.h"
#include "lvgl.h"
#include "lv_port_disp.h"
#include "my_mqtt.h"
#include "ui.h"

#define DBG_TAG "main"
#define DBG_LVL DBG_LOG

#define UI_THREAD_STACK_SIZE   4096
#define UI_THREAD_PRIORITY     20
#define UI_UPDATE_PERIOD_MS 500U
#define MQTT_THREAD_STACK_SIZE 4096
#define MQTT_THREAD_PRIORITY   22
#define MQTT_START_DELAY_MS    500U

/*
 * UI 线程是整个界面的“心跳”：
 * 1. lv_timer_handler() 让 LVGL 处理重绘、动画、定时器等内部任务。
 * 2. app_ui_update() 定期把 MQTT 收到的新数据刷新到屏幕对象上。
 *
 * 注意：这里不直接写 LCD 像素。真正刷屏会由 LVGL 调用
 * applications/lv_port_disp.c 里的 flush_cb 完成。
 */
static void ui_thread_entry(void *parameter)
{
    rt_tick_t last_update_tick = rt_tick_get();

    (void)parameter;

    while (1)
    {
        lv_timer_handler();

        if ((rt_tick_get() - last_update_tick) >=
            rt_tick_from_millisecond(UI_UPDATE_PERIOD_MS))
        {
            last_update_tick = rt_tick_get();
            app_ui_update();
        }

        rt_thread_mdelay(5);
    }
}

/*
 * MQTT 线程负责启动网络消息功能。
 * 延时一小段时间是为了让 RT-Thread 的设备、网络栈、AT 客户端等
 * 初始化链路先跑起来，避免 MQTT 过早连接失败。
 */
static void mqtt_thread_entry(void *parameter)
{
    (void)parameter;

    rt_thread_mdelay(MQTT_START_DELAY_MS);
    my_mqtt_start();
}

/*
 * 对 rt_thread_create() 做一层小封装：
 * main() 只需要说明线程名、入口、栈大小和优先级，
 * 失败时统一打印日志，避免 main() 里堆很多重复判断。
 */
static void create_app_thread(const char *name,
                              void (*entry)(void *parameter),
                              rt_uint32_t stack_size,
                              rt_uint8_t priority)
{
    rt_thread_t tid;

    tid = rt_thread_create(name, entry, RT_NULL, stack_size, priority, 20);
    if (tid != RT_NULL)
    {
        rt_thread_startup(tid);
    }
    else
    {
        LOG_E("create %s thread failed", name);
    }
}

int main(void)
{
    rt_thread_mdelay(500);

    LOG_D("Hello RT-Thread!");

    /*
     * 启动顺序很关键：
     * 1. LCD_Init() 先把 ILI9341 屏幕控制器初始化好。
     * 2. lv_init() 初始化 LVGL 内核。
     * 3. lv_port_disp_init() 把 LVGL 和 LCD 驱动接起来。
     * 4. app_ui_create() 创建页面、标签、进度条等 LVGL 对象。
     */
    LCD_Init();

    lv_init();
    lv_port_disp_init();

    app_ui_create();

    create_app_thread("ui", ui_thread_entry,
                      UI_THREAD_STACK_SIZE, UI_THREAD_PRIORITY);
    create_app_thread("mqtt", mqtt_thread_entry,
                      MQTT_THREAD_STACK_SIZE, MQTT_THREAD_PRIORITY);

    return RT_EOK;
}
