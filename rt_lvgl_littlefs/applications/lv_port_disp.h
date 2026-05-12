#ifndef LV_PORT_DISP_H
#define LV_PORT_DISP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

/* 初始化 LVGL 显示驱动，把 LVGL 的 flush 回调接到本工程的 LCD 驱动。 */
void lv_port_disp_init(void);

/* 获取已注册的 LVGL 显示对象；后续如果要设置默认屏幕可用它。 */
lv_disp_t * lv_port_disp_get(void);

/* 预留接口。当前是阻塞式刷屏，所以这里不需要轮询 DMA 状态。 */
void lv_port_disp_handler(void);

#ifdef __cplusplus
}
#endif

#endif /* LV_PORT_DISP_H */
