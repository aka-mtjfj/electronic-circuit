#include <rtthread.h>
#include <time.h>

#include "Lcd.h"
#include "lvgl.h"
#include "lvgl/src/widgets/lv_img.h"
#include "my_mqtt.h"
#include "ui.h"

#define UI_HEARTBEAT_PERIOD_MS 1000U
#define UI_HEARTBEAT_ON_MS     500U
#define UI_NAV_H               44
#define UI_MARGIN              8
#define UI_MSG_COUNT           10
#define UI_TOPIC_BUF_SIZE      24
#define UI_PAYLOAD_BUF_SIZE    96
#define UI_MSG_ITEM_GAP        4
#define UI_MSG_X               8
#define UI_MSG_Y               44
#define UI_MSG_W               (LCD_WIDTH - 32)

/*
 * 这里声明的是 my_mqtt.c 提供给 UI 层读取的接口。
 * UI 不直接处理 MQTT 报文；它只拿“已经整理好的历史消息和 PC 状态”。
 */
extern rt_uint32_t my_mqtt_get_message_seq(void);
extern rt_size_t my_mqtt_get_history_count(void);
extern rt_bool_t my_mqtt_get_history_item(rt_size_t display_index,
                                          char *topic, rt_size_t topic_size,
                                          char *payload, rt_size_t payload_size,
                              rt_bool_t *is_new);

extern const lv_img_dsc_t home_avatar;
extern const lv_font_t ui_font_zh_14;

#define UI_TEXT_FONT (&ui_font_zh_14)

/*
 * 这些全局指针保存已经创建好的 LVGL 控件。
 * 后续刷新时只改控件内容，比如 label 文本、进度条宽度、颜色；
 * 不在每次刷新时重新创建对象，避免内存碎片和界面闪烁。
 */
static lv_obj_t *g_heartbeat_boxes[3];
static lv_obj_t *g_topic_labels[UI_MSG_COUNT];
static lv_obj_t *g_payload_line1_labels[UI_MSG_COUNT];
static lv_obj_t *g_payload_line2_labels[UI_MSG_COUNT];
static lv_obj_t *g_pages[3];
static lv_obj_t *g_active_title;
static lv_obj_t *g_active_lines[5];
static lv_obj_t *g_active_marks[3];
static lv_obj_t *g_nav_boxes[3];
static lv_obj_t *g_nav_labels[3];
static lv_obj_t *g_home_title;
static lv_obj_t *g_panel_net_name;
static lv_obj_t *g_panel_battery_name;
static lv_obj_t *g_panel_cpu_value;
static lv_obj_t *g_panel_cpu_bar;
static lv_obj_t *g_panel_mem_value;
static lv_obj_t *g_panel_mem_bar;
static lv_obj_t *g_panel_net_value;
static lv_obj_t *g_panel_ip_value;
static lv_obj_t *g_panel_battery_value;
static lv_obj_t *g_panel_date_value;
static lv_obj_t *g_panel_time_value;
static lv_obj_t *g_panel_pc_value;
static app_ui_page_t g_current_page = APP_UI_PAGE_HOME;
static app_ui_active_t g_current_active = APP_UI_ACTIVE_OPTIONS;
static volatile app_ui_page_t g_requested_page = APP_UI_PAGE_HOME;
static volatile rt_bool_t g_page_request_pending = RT_FALSE;
static volatile app_ui_active_t g_requested_active = APP_UI_ACTIVE_OPTIONS;
static volatile rt_bool_t g_active_request_pending = RT_FALSE;
static rt_uint32_t g_seen_message_seq = (rt_uint32_t)-1;
static rt_uint32_t g_seen_pc_status_seq = (rt_uint32_t)-1;
static rt_bool_t g_seen_pc_online = RT_FALSE;

static lv_obj_t *ui_line(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                         lv_coord_t w, lv_coord_t h);
static void ui_create_heartbeat(app_ui_page_t page_id, lv_obj_t *parent);
static void ui_create_message_page(lv_obj_t *parent);
static void ui_create_active_page(lv_obj_t *parent);
static void ui_create_home_page(lv_obj_t *parent);
static void ui_create_nav(lv_obj_t *parent);
static lv_obj_t *ui_create_label(lv_obj_t *parent, const char *text,
                                 lv_coord_t x, lv_coord_t y);
static lv_obj_t *ui_create_meter_row(lv_obj_t *parent, const char *name,
                                     const char *value, rt_uint8_t percent,
                                     lv_coord_t y, lv_obj_t **value_label);
static void ui_set_bar_percent(lv_obj_t *bar, rt_uint8_t percent);
static void ui_format_rate(char *buf, rt_size_t size, rt_uint32_t bytes_per_sec);
static void ui_update_datetime(void);
static lv_coord_t ui_msg_item_height(void);
static void ui_copy_label_text(char *dst, rt_size_t dst_size, const char *src);
static void ui_update_nav(void);
static void ui_update_heartbeat(void);
static void ui_update_message(void);
static void ui_update_home(void);
static void ui_apply_active_font(void);
static void ui_apply_home_lang(void);

void app_ui_create(void)
{
    /*
     * app_ui_create() 只在 main.c 初始化阶段调用一次。
     * 它负责搭好三个页面：消息页、首页、选项页，以及底部导航栏。
     */
    lv_obj_t *scr = lv_scr_act();

    lv_obj_set_style_bg_color(scr, lv_color_hex(0xF7F7F2), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(scr, lv_color_hex(0x101010), LV_PART_MAIN);
    lv_obj_set_style_text_font(scr, UI_TEXT_FONT, LV_PART_MAIN);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    ui_create_message_page(scr);
    ui_create_home_page(scr);
    ui_create_active_page(scr);
    ui_create_nav(scr);
    app_ui_set_page(APP_UI_PAGE_HOME);
    app_ui_update();
}

void app_ui_update(void)
{
    /*
     * MQTT 线程不能直接操作 LVGL 控件。
     * 所以跨线程来的页面切换请求先记录到 g_requested_xxx，
     * 最后统一在 UI 线程的 app_ui_update() 里执行。
     */
    if (g_active_request_pending)
    {
        app_ui_set_active((app_ui_active_t)g_requested_active);
        g_active_request_pending = RT_FALSE;
    }

    if (g_page_request_pending)
    {
        app_ui_set_page((app_ui_page_t)g_requested_page);
        g_page_request_pending = RT_FALSE;
    }

    ui_update_heartbeat();
    ui_update_message();
    ui_update_home();
}

void app_ui_set_page(app_ui_page_t page)
{
    int i;

    if (page > APP_UI_PAGE_OPTIONS)
    {
        return;
    }

    g_current_page = page;

    for (i = 0; i < 3; ++i)
    {
        if (g_pages[i] == RT_NULL)
        {
            continue;
        }

        if (i == (int)page)
        {
            lv_obj_clear_flag(g_pages[i], LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(g_pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    ui_update_nav();
}

void app_ui_request_page(app_ui_page_t page)
{
    if (page > APP_UI_PAGE_OPTIONS)
    {
        return;
    }

    g_requested_page = page;
    g_page_request_pending = RT_TRUE;
}


static void ui_apply_active_font(void)
{
    int i;

    if (g_active_title != RT_NULL)
    {
            lv_obj_set_style_text_font(g_active_title, UI_TEXT_FONT, LV_PART_MAIN);
    }
    for (i = 0; i < 5; ++i)
    {
        if (g_active_lines[i] != RT_NULL)
        {
            lv_obj_set_style_text_font(g_active_lines[i], UI_TEXT_FONT, LV_PART_MAIN);
        }
    }
}
void app_ui_set_active(app_ui_active_t active)
{
    int i;

    if (active > APP_UI_ACTIVE_ABOUT)
    {
        return;
    }

    g_current_active = active;
    if (g_active_title == RT_NULL)
    {
        return;
    }

    for (i = 0; i < 3; ++i)
    {
        lv_obj_add_flag(g_active_marks[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (i = 0; i < 5; ++i)
    {
        lv_obj_set_width(g_active_lines[i], LCD_WIDTH - 36);
        lv_obj_align(g_active_lines[i], LV_ALIGN_TOP_LEFT, 6,
                     (lv_coord_t)(38 + i * 28));
        lv_label_set_text(g_active_lines[i], "");
    }
    ui_apply_active_font();

    if (active == APP_UI_ACTIVE_SETTING)
    {
        lv_label_set_text(g_active_title, "设置");
        lv_label_set_text(g_active_lines[0], "界面语言: 中文");
        lv_label_set_text(g_active_lines[1], "显示: 320x240");
        lv_label_set_text(g_active_lines[2], "存储: littlefs");
        lv_label_set_text(g_active_lines[3], "输入: MQTT命令");
        lv_label_set_text(g_active_lines[4], "");
    }
    else if (active == APP_UI_ACTIVE_ABOUT)
    {
        lv_label_set_text(g_active_title, "关于");
        lv_label_set_text(g_active_lines[0], "设备: rt_lvgl_littlefs");
        lv_label_set_text(g_active_lines[1], "系统: RT-Thread");
        lv_label_set_text(g_active_lines[2], "界面: LVGL + MQTT");
        lv_label_set_text(g_active_lines[3], "输入: 切换命令");
        lv_label_set_text(g_active_lines[4], "构建: " __DATE__);
    }
    else
    {
        static const lv_color_t mark_colors[3] = {
            LV_COLOR_MAKE(0xFF, 0x99, 0x00),
            LV_COLOR_MAKE(0x22, 0xC5, 0x5E),
            LV_COLOR_MAKE(0x3B, 0x82, 0xF6),
        };

        lv_label_set_text(g_active_title, "功能选择");
        lv_label_set_text(g_active_lines[0], "设置");
        lv_label_set_text(g_active_lines[1], "设备信息");
        lv_label_set_text(g_active_lines[2], "番茄钟");
        for (i = 0; i < 3; ++i)
        {
            lv_obj_clear_flag(g_active_marks[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(g_active_marks[i], mark_colors[i],
                                      LV_PART_MAIN);
            lv_obj_align(g_active_lines[i], LV_ALIGN_TOP_LEFT, 24,
                         (lv_coord_t)(38 + i * 28));
            lv_obj_set_width(g_active_lines[i], LCD_WIDTH - 54);
        }
    }
}

void app_ui_request_active(app_ui_active_t active)
{
    if (active > APP_UI_ACTIVE_ABOUT)
    {
        return;
    }

    g_requested_active = active;
    g_active_request_pending = RT_TRUE;
}

static lv_obj_t *ui_line(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                         lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *line = lv_obj_create(parent);

    lv_obj_remove_style_all(line);
    lv_obj_set_pos(line, x, y);
    lv_obj_set_size(line, w, h);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x101010), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, LV_PART_MAIN);

    return line;
}

static void ui_create_heartbeat(app_ui_page_t page_id, lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);

    g_heartbeat_boxes[page_id] = box;
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 12, 12);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x111111), LV_PART_MAIN);
    lv_obj_align(box, LV_ALIGN_TOP_RIGHT, -14, 16);
}

static void ui_create_message_page(lv_obj_t *parent)
{
    /*
     * 消息页显示 MQTT 命令和回复历史。
     * 左侧小色块/符号表示方向或错误，右侧显示 payload。
     */
    lv_obj_t *page;
    lv_obj_t *title;

    page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_pos(page, UI_MARGIN + 2, 6);
    lv_obj_set_size(page, LCD_WIDTH - 2 * UI_MARGIN - 4, LCD_HEIGHT - UI_NAV_H - 8);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    g_pages[APP_UI_PAGE_MESSAGE] = page;

    title = lv_label_create(page);
    lv_label_set_text(title, "消息");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 6, 10);

    ui_create_heartbeat(APP_UI_PAGE_MESSAGE, page);

    ui_line(parent, UI_MARGIN, 4, LCD_WIDTH - 2 * UI_MARGIN, 2);
    ui_line(parent, UI_MARGIN, 4, 2, LCD_HEIGHT - UI_NAV_H - 4);
    ui_line(parent, LCD_WIDTH - UI_MARGIN - 2, 4, 2, LCD_HEIGHT - UI_NAV_H - 4);
    ui_line(parent, UI_MARGIN, LCD_HEIGHT - UI_NAV_H, LCD_WIDTH - 2 * UI_MARGIN, 2);

    for (int i = 0; i < UI_MSG_COUNT; ++i)
    {
        lv_coord_t item_h = ui_msg_item_height();
        lv_coord_t line_h = UI_TEXT_FONT->line_height;

        lv_coord_t y = (lv_coord_t)(UI_MSG_Y + i * (item_h + UI_MSG_ITEM_GAP));

        g_topic_labels[i] = lv_label_create(page);
        lv_label_set_long_mode(g_topic_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_size(g_topic_labels[i], 14, line_h);
        lv_obj_set_style_text_color(g_topic_labels[i], lv_color_hex(0xFF9900),
                                    LV_PART_MAIN);
        lv_obj_clear_flag(g_topic_labels[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(g_topic_labels[i], LV_ALIGN_TOP_LEFT, UI_MSG_X, y);

        g_payload_line1_labels[i] = lv_label_create(page);
        lv_label_set_long_mode(g_payload_line1_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_size(g_payload_line1_labels[i], UI_MSG_W - 18, line_h);
        lv_obj_set_style_text_color(g_payload_line1_labels[i], lv_color_hex(0x22C55E),
                                    LV_PART_MAIN);
        lv_obj_clear_flag(g_payload_line1_labels[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(g_payload_line1_labels[i], LV_ALIGN_TOP_LEFT,
                     UI_MSG_X + 18, y);

        g_payload_line2_labels[i] = lv_label_create(page);
        lv_label_set_long_mode(g_payload_line2_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_size(g_payload_line2_labels[i], UI_MSG_W, line_h);
        lv_obj_clear_flag(g_payload_line2_labels[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(g_payload_line2_labels[i], LV_ALIGN_TOP_LEFT, UI_MSG_X, y);
        lv_label_set_text(g_payload_line2_labels[i], "");
    }
}

static void ui_create_active_page(lv_obj_t *parent)
{
    /* 选项页本身不复杂：标题 + 若干行文字 + 左侧彩色标记。 */
    lv_obj_t *page;

    page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_pos(page, UI_MARGIN + 2, 6);
    lv_obj_set_size(page, LCD_WIDTH - 2 * UI_MARGIN - 4, LCD_HEIGHT - UI_NAV_H - 8);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    g_pages[APP_UI_PAGE_OPTIONS] = page;

    g_active_title = lv_label_create(page);
    lv_obj_set_width(g_active_title, LCD_WIDTH - 48);
    lv_label_set_long_mode(g_active_title, LV_LABEL_LONG_CLIP);
    lv_obj_align(g_active_title, LV_ALIGN_TOP_LEFT, 6, 10);

    for (int i = 0; i < 3; ++i)
    {
        g_active_marks[i] = lv_obj_create(page);
        lv_obj_remove_style_all(g_active_marks[i]);
        lv_obj_set_size(g_active_marks[i], 10, 10);
        lv_obj_set_style_bg_opa(g_active_marks[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_align(g_active_marks[i], LV_ALIGN_TOP_LEFT, 8,
                     (lv_coord_t)(42 + i * 28));
    }

    for (int i = 0; i < 5; ++i)
    {
        g_active_lines[i] = lv_label_create(page);
        lv_obj_set_width(g_active_lines[i], LCD_WIDTH - 36);
        lv_label_set_long_mode(g_active_lines[i], LV_LABEL_LONG_CLIP);
        lv_obj_align(g_active_lines[i], LV_ALIGN_TOP_LEFT, 6,
                     (lv_coord_t)(38 + i * 28));
    }

    ui_create_heartbeat(APP_UI_PAGE_OPTIONS, page);
    app_ui_set_active(APP_UI_ACTIVE_OPTIONS);
}

static lv_obj_t *ui_create_label(lv_obj_t *parent, const char *text,
                                 lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *label = lv_label_create(parent);

    lv_label_set_text(label, text);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, x, y);

    return label;
}

static lv_obj_t *ui_create_meter_row(lv_obj_t *parent, const char *name,
                                     const char *value, rt_uint8_t percent,
                                     lv_coord_t y, lv_obj_t **value_label)
{
    /*
     * 创建一行“名称 + 数值 + 进度条”。
     * CPU 和内存两行都用这个函数，返回值是进度条前景对象，
     * 后续 ui_set_bar_percent() 只需要改它的宽度。
     */
    lv_obj_t *label;
    lv_obj_t *bar_bg;
    lv_obj_t *bar_fill;
    lv_coord_t bar_w = LCD_WIDTH - 52;
    lv_coord_t fill_w;

    if (percent > 100)
    {
        percent = 100;
    }

    label = ui_create_label(parent, name, 6, y);
    lv_obj_set_style_text_color(label, lv_color_hex(0x101010), LV_PART_MAIN);

    label = ui_create_label(parent, value, 72, y);
    lv_obj_set_style_text_color(label, lv_color_hex(0x101010), LV_PART_MAIN);
    if (value_label != RT_NULL)
    {
        *value_label = label;
    }

    bar_bg = lv_obj_create(parent);
    lv_obj_remove_style_all(bar_bg);
    lv_obj_set_pos(bar_bg, 6, (lv_coord_t)(y + 20));
    lv_obj_set_size(bar_bg, bar_w, 8);
    lv_obj_set_style_bg_opa(bar_bg, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_bg, lv_color_hex(0xCFCFCF), LV_PART_MAIN);

    fill_w = (lv_coord_t)((bar_w * percent) / 100);
    bar_fill = lv_obj_create(parent);
    lv_obj_remove_style_all(bar_fill);
    lv_obj_set_pos(bar_fill, 6, (lv_coord_t)(y + 20));
    lv_obj_set_size(bar_fill, fill_w, 8);
    lv_obj_set_style_bg_opa(bar_fill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_fill, lv_color_hex(0xFF9900), LV_PART_MAIN);

    return bar_fill;
}

static void ui_set_bar_percent(lv_obj_t *bar, rt_uint8_t percent)
{
    lv_coord_t bar_w = LCD_WIDTH - 52;

    if (bar == RT_NULL)
    {
        return;
    }
    if (percent > 100)
    {
        percent = 100;
    }

    lv_obj_set_width(bar, (lv_coord_t)((bar_w * percent) / 100));
}

static void ui_format_rate(char *buf, rt_size_t size, rt_uint32_t bytes_per_sec)
{
    /* 把字节每秒转换成适合小屏显示的 B/s、kB/s、MB/s、GB/s。 */
    const char *unit = "B/s";
    rt_uint32_t value = bytes_per_sec;

    if (buf == RT_NULL || size == 0)
    {
        return;
    }

    if (bytes_per_sec >= 1024U * 1024U * 1024U)
    {
        value = bytes_per_sec / (1024U * 1024U * 1024U);
        unit = "GB/s";
    }
    else if (bytes_per_sec >= 1024U * 1024U)
    {
        value = bytes_per_sec / (1024U * 1024U);
        unit = "MB/s";
    }
    else if (bytes_per_sec >= 1024U)
    {
        value = bytes_per_sec / 1024U;
        unit = "kB/s";
    }

    rt_snprintf(buf, size, "%u%s", value, unit);
}

static void ui_update_datetime(void)
{
    /*
     * RT-Thread 的 time() 如果还没被 NTP/RTC 校准，可能还是接近 0。
     * 这里用 now < 24h 作为“时间还没准备好”的简单判断。
     */
    time_t now;
    struct tm *tm_now;
    char text[24];

    now = time(RT_NULL);
    tm_now = localtime(&now);
    if (tm_now == RT_NULL || now < 24 * 60 * 60)
    {
        lv_label_set_text(g_panel_date_value, "日期: --");
        lv_label_set_text(g_panel_time_value, "时间: --:--");
        return;
    }

    rt_snprintf(text, sizeof(text), "日期: %04d-%02d-%02d",
                tm_now->tm_year + 1900,
                tm_now->tm_mon + 1,
                tm_now->tm_mday);
    lv_label_set_text(g_panel_date_value, text);

    rt_snprintf(text, sizeof(text), "时间: %02d:%02d",
                tm_now->tm_hour,
                tm_now->tm_min);
    lv_label_set_text(g_panel_time_value, text);
}

static void ui_create_home_page(lv_obj_t *parent)
{
    /*
     * 首页是这个项目的状态看板：
     * 日期时间、CPU、内存、网络速率、IP、电量、PC 在线状态都在这里。
     */
    lv_obj_t *page;
    lv_obj_t *label;

    page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_pos(page, UI_MARGIN + 2, 6);
    lv_obj_set_size(page, LCD_WIDTH - 2 * UI_MARGIN - 4, LCD_HEIGHT - UI_NAV_H - 8);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    g_pages[APP_UI_PAGE_HOME] = page;

    g_home_title = ui_create_label(page, "首页", 6, 10);
    lv_obj_set_style_text_color(g_home_title, lv_color_hex(0x101010), LV_PART_MAIN);

    ui_create_heartbeat(APP_UI_PAGE_HOME, page);

    label = lv_img_create(page);
    lv_img_set_src(label, &home_avatar);
    lv_obj_align(label, LV_ALIGN_TOP_RIGHT, -34, 8);

    g_panel_date_value = ui_create_label(page, "日期: --", 6, 32);
    lv_obj_set_style_text_color(g_panel_date_value, lv_color_hex(0x22C55E),
                                LV_PART_MAIN);
    g_panel_time_value = ui_create_label(page, "时间: --:--", 6, 50);
    lv_obj_set_style_text_color(g_panel_time_value, lv_color_hex(0x22C55E),
                                LV_PART_MAIN);

    g_panel_cpu_bar = ui_create_meter_row(page, "处理器", "--%", 0, 76,
                                          &g_panel_cpu_value);
    g_panel_mem_bar = ui_create_meter_row(page, "内存", "--%", 0, 122,
                                          &g_panel_mem_value);

    g_panel_net_name = ui_create_label(page, "网络", 6, 170);
    lv_obj_set_style_text_color(g_panel_net_name, lv_color_hex(0x101010), LV_PART_MAIN);
    g_panel_net_value = ui_create_label(page, "下载:--B/s  上传:--B/s", 56, 170);
    lv_obj_set_style_text_color(g_panel_net_value, lv_color_hex(0x101010),
                                LV_PART_MAIN);

    label = ui_create_label(page, "地址", 6, 194);
    lv_obj_set_style_text_color(label, lv_color_hex(0x101010), LV_PART_MAIN);
    g_panel_ip_value = ui_create_label(page, "--", 56, 194);
    lv_obj_set_style_text_color(g_panel_ip_value, lv_color_hex(0x101010),
                                LV_PART_MAIN);

    g_panel_battery_name = ui_create_label(page, "电量", 6, 216);
    lv_obj_set_style_text_color(g_panel_battery_name, lv_color_hex(0x101010), LV_PART_MAIN);
    g_panel_battery_value = ui_create_label(page, "--%", 56, 216);
    lv_obj_set_style_text_color(g_panel_battery_value, lv_color_hex(0x101010),
                                LV_PART_MAIN);

    g_panel_pc_value = ui_create_label(page, "电脑: 离线", 6, 238);
    lv_obj_set_style_text_color(g_panel_pc_value, lv_color_hex(0x808080),
                                LV_PART_MAIN);
    ui_apply_home_lang();
}

static void ui_create_nav(lv_obj_t *parent)
{
    /* 底部三段导航栏：消息 / 首页 / 选项。 */
    static const char *names[] = {"消息", "首页", "选项"};
    lv_coord_t y = LCD_HEIGHT - UI_NAV_H;
    lv_coord_t x = UI_MARGIN;
    lv_coord_t w = LCD_WIDTH - 2 * UI_MARGIN;
    lv_coord_t tab_w = w / 3;
    int i;

    ui_line(parent, x, y, w, 2);
    ui_line(parent, UI_MARGIN, y, 2, UI_NAV_H - 2);
    ui_line(parent, LCD_WIDTH - UI_MARGIN - 2, y, 2, UI_NAV_H - 2);

    for (i = 0; i < 3; ++i)
    {
        lv_obj_t *box = lv_obj_create(parent);
        lv_obj_t *label = lv_label_create(box);

        g_nav_boxes[i] = box;
        lv_obj_remove_style_all(box);
        lv_obj_set_pos(box, (lv_coord_t)(x + 2 + i * tab_w), y + 2);
        lv_obj_set_size(box, (i == 2) ? (w - i * tab_w - 4) : (tab_w - 2),
                        UI_NAV_H - 4);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(box, lv_color_hex(0x808080), LV_PART_MAIN);

        g_nav_labels[i] = label;
        lv_label_set_text(label, names[i]);
        lv_obj_set_width(label, tab_w - 10);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_center(label);

        if (i > 0)
        {
            ui_line(parent, (lv_coord_t)(x + i * tab_w), y, 2, UI_NAV_H - 2);
        }
    }
}

static void ui_update_nav(void)
{
    int i;

    for (i = 0; i < 3; ++i)
    {
        if (g_nav_labels[i] == RT_NULL)
        {
            continue;
        }

        if (g_nav_boxes[i] != RT_NULL)
        {
            lv_obj_set_style_bg_color(g_nav_boxes[i],
                                      (i == (int)g_current_page) ?
                                      lv_color_hex(0xFF9900) :
                                      lv_color_hex(0x808080),
                                      LV_PART_MAIN);
        }

        lv_obj_set_style_text_color(g_nav_labels[i], lv_color_hex(0x101010),
                                    LV_PART_MAIN);
    }
}

static lv_coord_t ui_msg_item_height(void)
{
    return UI_TEXT_FONT->line_height;
}

static void ui_copy_label_text(char *dst, rt_size_t dst_size, const char *src)
{
    /*
     * MQTT payload 可能包含换行、# 等对当前简易显示格式不友好的字符。
     * 这里统一替换成空格，并且尽量不要截断在 UTF-8 中文字符中间。
     */
    rt_size_t i;
    rt_size_t copy_len;

    if (dst == RT_NULL || dst_size == 0)
    {
        return;
    }

    copy_len = rt_strlen(src);
    if (copy_len >= dst_size)
    {
        copy_len = dst_size - 1;
    }

    while (copy_len > 0 && (((const rt_uint8_t *)src)[copy_len] & 0xC0) == 0x80)
    {
        copy_len--;
    }

    for (i = 0; i < copy_len; ++i)
    {
        if (src[i] == '#' || src[i] == '\r' || src[i] == '\n')
        {
            dst[i] = ' ';
        }
        else
        {
            dst[i] = src[i];
        }
    }

    dst[i] = '\0';
}

static void ui_update_heartbeat(void)
{
    /* 三个页面右上角的小方块闪烁，用来表示 UI 线程还在正常跑。 */
    rt_uint32_t phase = rt_tick_get_millisecond() % UI_HEARTBEAT_PERIOD_MS;

    if (phase < UI_HEARTBEAT_ON_MS)
    {
        for (int i = 0; i < 3; ++i)
        {
            lv_obj_set_style_bg_color(g_heartbeat_boxes[i], lv_color_hex(0x22C55E),
                                      LV_PART_MAIN);
        }
    }
    else
    {
        for (int i = 0; i < 3; ++i)
        {
            lv_obj_set_style_bg_color(g_heartbeat_boxes[i], lv_color_hex(0x101010),
                                      LV_PART_MAIN);
        }
    }
}

static void ui_update_message(void)
{
    /*
     * 只有 MQTT 消息序号变化时才刷新消息页。
     * 这样避免每 500ms 重复设置所有 label，减少小屏刷新压力。
     */
    rt_uint32_t seq = my_mqtt_get_message_seq();
    rt_size_t count;
    rt_size_t visible_count;

    if (seq == g_seen_message_seq)
    {
        return;
    }

    count = my_mqtt_get_history_count();
    visible_count = (count > UI_MSG_COUNT) ? UI_MSG_COUNT : count;
    for (int i = 0; i < UI_MSG_COUNT; ++i)
    {
        rt_size_t display_index;
        char prefix[UI_TOPIC_BUF_SIZE];
        char text[UI_PAYLOAD_BUF_SIZE];
        char safe_text[UI_PAYLOAD_BUF_SIZE];
        rt_bool_t is_new = RT_FALSE;

        if ((rt_size_t)i >= visible_count)
        {
            lv_label_set_text(g_topic_labels[i], "");
            lv_label_set_text(g_payload_line1_labels[i], "");
            lv_label_set_text(g_payload_line2_labels[i], "");
            continue;
        }

        display_index = visible_count - 1 - (rt_size_t)i;
        if (!my_mqtt_get_history_item(display_index, prefix, sizeof(prefix),
                                      text, sizeof(text), &is_new))
        {
            lv_label_set_text(g_topic_labels[i], "");
            lv_label_set_text(g_payload_line1_labels[i], "");
            lv_label_set_text(g_payload_line2_labels[i], "");
            continue;
        }

        ui_copy_label_text(safe_text, sizeof(safe_text), text);
        lv_obj_set_style_text_color(g_topic_labels[i],
                                    prefix[0] == '>' ? lv_color_hex(0xFF9900) :
                                    prefix[0] == '!' ? lv_color_hex(0xD00000) :
                                                        lv_color_hex(0x22C55E),
                                    LV_PART_MAIN);
        lv_obj_set_style_text_color(g_payload_line1_labels[i],
                                    is_new ? lv_color_hex(0x101010) :
                                             lv_color_hex(0x606060),
                                    LV_PART_MAIN);

        lv_label_set_text(g_topic_labels[i], prefix);
        lv_label_set_text(g_payload_line1_labels[i], safe_text);
        lv_label_set_text(g_payload_line2_labels[i], "");
    }

    g_seen_message_seq = seq;
}

static void ui_apply_home_lang(void)
{
    if (g_home_title != RT_NULL)
    {
        lv_obj_set_style_text_font(g_home_title, UI_TEXT_FONT, LV_PART_MAIN);
        lv_label_set_text(g_home_title, "首页");
    }
    if (g_panel_date_value != RT_NULL)
    {
        lv_obj_set_style_text_font(g_panel_date_value, UI_TEXT_FONT, LV_PART_MAIN);
    }
    if (g_panel_time_value != RT_NULL)
    {
        lv_obj_set_style_text_font(g_panel_time_value, UI_TEXT_FONT, LV_PART_MAIN);
    }
    if (g_panel_net_name != RT_NULL)
    {
        lv_obj_set_style_text_font(g_panel_net_name, UI_TEXT_FONT, LV_PART_MAIN);
        lv_label_set_text(g_panel_net_name, "网络");
    }
    if (g_panel_battery_name != RT_NULL)
    {
        lv_obj_set_style_text_font(g_panel_battery_name, UI_TEXT_FONT, LV_PART_MAIN);
        lv_label_set_text(g_panel_battery_name, "电量");
    }
    if (g_panel_pc_value != RT_NULL)
    {
        lv_obj_set_style_text_font(g_panel_pc_value, UI_TEXT_FONT, LV_PART_MAIN);
        lv_label_set_text(g_panel_pc_value, "电脑: 离线");
    }
}
static void ui_update_home(void)
{
    /*
     * 首页读取 my_mqtt.c 保存的 PC 状态。
     * 如果 10 秒内没有新状态，就认为 PC 离线，界面显示灰色离线状态。
     */
    my_mqtt_pc_status_t status;
    char text[64];
    char down_text[16];
    char up_text[16];
    rt_uint32_t age_ms;
    rt_bool_t online;

    ui_update_datetime();

    my_mqtt_get_pc_status(&status);
    age_ms = rt_tick_get_millisecond() - status.last_tick_ms;
    online = (status.online && age_ms < 10000U) ? RT_TRUE : RT_FALSE;

    if (status.seq == g_seen_pc_status_seq && online == g_seen_pc_online)
    {
        return;
    }

    rt_snprintf(text, sizeof(text), "%u%%", status.cpu);
    lv_label_set_text(g_panel_cpu_value, text);
    ui_set_bar_percent(g_panel_cpu_bar, status.cpu);

    rt_snprintf(text, sizeof(text), "%u%%", status.mem);
    lv_label_set_text(g_panel_mem_value, text);
    ui_set_bar_percent(g_panel_mem_bar, status.mem);

    ui_format_rate(down_text, sizeof(down_text), status.down);
    ui_format_rate(up_text, sizeof(up_text), status.up);
    rt_snprintf(text, sizeof(text), "下载:%s  上传:%s", down_text, up_text);
    lv_label_set_text(g_panel_net_value, text);

    rt_snprintf(text, sizeof(text), "%s", status.ip);
    lv_label_set_text(g_panel_ip_value, text);

    if (status.battery < 0)
    {
        lv_label_set_text(g_panel_battery_value, "--%");
    }
    else
    {
        rt_snprintf(text, sizeof(text), "%d%%", status.battery);
        lv_label_set_text(g_panel_battery_value, text);
    }

    lv_label_set_text(g_panel_pc_value, online ? "电脑: 在线" : "电脑: 离线");
    lv_obj_set_style_text_color(g_panel_pc_value,
                                online ? lv_color_hex(0x22C55E) :
                                         lv_color_hex(0x808080),
                                LV_PART_MAIN);

    g_seen_pc_status_seq = status.seq;
    g_seen_pc_online = online;
}







