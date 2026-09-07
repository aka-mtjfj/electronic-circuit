#include "lv_port_disp.h"

#include "Lcd.h"

#define LV_PORT_HOR_RES         LCD_WIDTH
#define LV_PORT_VER_RES         LCD_HEIGHT
#define LV_PORT_BUF_LINES       30U
#define LV_PORT_BUF_PIXELS      (LV_PORT_HOR_RES * LV_PORT_BUF_LINES)

/*
 * LVGL 不会每次都准备一整屏 240x320 的缓存。
 * 这里用“30 行高度”的小缓存，让 LVGL 分块渲染，再由 flush_cb
 * 一块一块送到 LCD。这样能明显节省 RAM。
 */
static lv_disp_draw_buf_t g_draw_buf;
static lv_color_t g_buf1[LV_PORT_BUF_PIXELS];
static lv_disp_drv_t g_disp_drv;
static lv_disp_t * g_disp = NULL;

static void lv_port_flush_cb(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p);

void lv_port_disp_init(void)
{
    /*
     * 第一步：告诉 LVGL 绘图缓存在哪里、有多少像素。
     * 第二个缓存传 NULL，表示当前只用单缓冲。
     */
    lv_disp_draw_buf_init(&g_draw_buf, g_buf1, NULL, LV_PORT_BUF_PIXELS);

    /*
     * 第二步：注册显示驱动。
     * hor_res/ver_res 是屏幕分辨率，flush_cb 是“把像素真正送到屏幕”的回调。
     */
    lv_disp_drv_init(&g_disp_drv);
    g_disp_drv.hor_res = LV_PORT_HOR_RES;
    g_disp_drv.ver_res = LV_PORT_VER_RES;
    g_disp_drv.flush_cb = lv_port_flush_cb;
    g_disp_drv.draw_buf = &g_draw_buf;

    g_disp = lv_disp_drv_register(&g_disp_drv);
}

lv_disp_t * lv_port_disp_get(void)
{
    return g_disp;
}

void lv_port_disp_handler(void)
{
    /*
     * 目前 LCD 写入是阻塞式 SPI 发送：LCD_WritePixels() 返回时，
     * 这一块像素已经发完了，所以这里没有 DMA busy 状态需要轮询。
     */
}

/*
 * LVGL 的真正刷屏出口。
 *
 * area    : LVGL 认为需要更新的矩形区域。
 * color_p : 这块区域对应的 RGB565 像素数据。
 *
 * 调用链大致是：
 * main.c 的 ui_thread_entry()
 *   -> lv_timer_handler()
 *   -> LVGL 内部发现有对象需要重绘
 *   -> lv_port_flush_cb()
 *   -> LCD_Set_Window() + LCD_WritePixels()
 */
static void lv_port_flush_cb(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p)
{
    uint32_t width;
    uint32_t height;
    uint32_t pixel_count;

    if((disp_drv == NULL) || (area == NULL) || (color_p == NULL)) {
        if(disp_drv != NULL) {
            lv_disp_flush_ready(disp_drv);
        }
        return;
    }

    if((area->x2 < 0) || (area->y2 < 0) ||
       (area->x1 >= LV_PORT_HOR_RES) || (area->y1 >= LV_PORT_VER_RES)) {
        lv_disp_flush_ready(disp_drv);
        return;
    }

    width = (uint32_t)(area->x2 - area->x1 + 1);
    height = (uint32_t)(area->y2 - area->y1 + 1);
    pixel_count = width * height;

    /* 先告诉 ILI9341 接下来要写哪一块窗口，再连续发送这块窗口的像素。 */
    LCD_Set_Window((uint16_t)area->x1, (uint16_t)area->y1, (uint16_t)area->x2, (uint16_t)area->y2);
    (void)LCD_WritePixels((const uint8_t *)color_p, pixel_count * 2U);

    /*
     * 必须通知 LVGL：这次 flush 已经完成。
     * 如果忘了这一句，LVGL 会一直以为屏幕还在忙，后续刷新可能卡住。
     */
    lv_disp_flush_ready(disp_drv);
}
