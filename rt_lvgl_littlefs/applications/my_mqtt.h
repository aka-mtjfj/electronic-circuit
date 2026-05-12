#ifndef __MY_MQTT_H__
#define __MY_MQTT_H__

#include <rtthread.h>

/*
 * 电脑状态数据结构。
 * Python 端或 PC 端通过 MQTT 上报 cpu/mem/down/up/bat/ip，
 * MCU 端解析后保存到这里，ui.c 再读取并显示到首页。
 */
typedef struct
{
    rt_uint8_t cpu;
    rt_uint8_t mem;
    rt_uint32_t down;
    rt_uint32_t up;
    rt_int16_t battery;
    char ip[16];
    rt_bool_t online;
    rt_uint32_t seq;
    rt_uint32_t last_tick_ms;
} my_mqtt_pc_status_t;

/* 启动 MQTT 客户端：连接服务器、订阅主题、进入消息循环。 */
int my_mqtt_start(void);

/* 给 UI 层读取最近一次 PC 状态。 */
void my_mqtt_get_pc_status(my_mqtt_pc_status_t *status);

#endif /* __MY_MQTT_H__ */
