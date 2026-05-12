#ifndef __APP_UI_H__
#define __APP_UI_H__

typedef enum
{
    /* 消息页：显示 MQTT 收到的命令、回复和错误提示。 */
    APP_UI_PAGE_MESSAGE = 0,

    /* 首页：显示日期时间、CPU/内存、网络速率、电量、PC 在线状态。 */
    APP_UI_PAGE_HOME,

    /* 选项页：显示设置、关于等二级页面。 */
    APP_UI_PAGE_OPTIONS,
} app_ui_page_t;

typedef enum
{
    /* 选项页入口。 */
    APP_UI_ACTIVE_OPTIONS = 0,

    /* 设置页。 */
    APP_UI_ACTIVE_SETTING,

    /* 关于页。 */
    APP_UI_ACTIVE_ABOUT,
} app_ui_active_t;

/* 创建所有 LVGL 页面和控件。只在 main.c 初始化阶段调用一次。 */
void app_ui_create(void);

/* 周期刷新页面内容，由 UI 线程定期调用。 */
void app_ui_update(void);

/* 立即切换页面；应在 UI 线程里调用。 */
void app_ui_set_page(app_ui_page_t page);

/* 请求切换页面；可由 MQTT 线程调用，真正切换会延迟到 UI 线程执行。 */
void app_ui_request_page(app_ui_page_t page);

/* 立即切换选项页内部内容；应在 UI 线程里调用。 */
void app_ui_set_active(app_ui_active_t active);

/* 请求切换选项页内部内容；可由 MQTT 线程调用。 */
void app_ui_request_active(app_ui_active_t active);

#endif /* __APP_UI_H__ */

