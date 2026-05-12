#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <rtthread.h>
#include <rthw.h>
#include <dfs_posix.h>
#include <libc/libc_fcntl.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include "paho_mqtt.h"
#include "my_mqtt.h"
#include "ui.h"

#define MQTT_NOTIFY_URI      "tcp://bemfa.com:9501"
#define MQTT_TOPIC_NOTIFY    "rtstm32001"
#define MQTT_TOPIC_PC_STATUS "rtpcstatus001"
#define MQTT_TOPIC_TODO      "rttodo001"
#define MQTT_NOTIFY_CLIENTID "e683645fd0944bd9a2a8b450de95f762"
#define MQTT_HISTORY_COUNT   12
#define MQTT_TOPIC_SIZE      24
#define MQTT_PAYLOAD_SIZE    96
#define MQTT_HEARTBEAT_MS    5000U
#define MQTT_HEARTBEAT_STACK 1024
#define MQTT_HEARTBEAT_PRIO  25

#define HTTP_GET_BUF_SIZE 768

/*
 * 建立一个最小 HTTP GET 用的 TCP 连接。
 * 支持直接传 IP，也支持传域名；域名会通过 gethostbyname() 解析。
 */
static int http_get_connect(const char *host, int port)
{
    int sock;
    struct sockaddr_in server_addr;
    struct hostent *host_entry;

    rt_memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((uint16_t)port);

    if (inet_aton(host, &server_addr.sin_addr) == 0)
    {
        host_entry = gethostbyname(host);
        if (host_entry == RT_NULL || host_entry->h_addr == RT_NULL)
        {
            rt_kprintf("http_get: unknown host %s\n", host);
            return -1;
        }
        rt_memcpy(&server_addr.sin_addr, host_entry->h_addr,
                  sizeof(server_addr.sin_addr));
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        rt_kprintf("http_get: socket failed\n");
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&server_addr,
                sizeof(server_addr)) < 0)
    {
        rt_kprintf("http_get: connect failed\n");
        close(sock);
        return -1;
    }

    return sock;
}

static char *http_get_find_body(char *buf, int len)
{
    /*
     * HTTP 响应头和正文之间用空行分隔：\r\n\r\n。
     * 找到这个位置后，后面的内容就是文件正文。
     */
    int i;

    for (i = 0; i <= len - 4; ++i)
    {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n')
        {
            return &buf[i + 4];
        }
    }

    return RT_NULL;
}

static int http_get_save_file(const char *host, int port,
                              const char *remote_path,
                              const char *local_path)
{
    /*
     * 这个函数把远端 HTTP 文件下载到本地 littlefs/DFS 文件系统。
     * 典型命令：
     * http_get 192.168.10.9 8000 /test.bin /test.bin
     */
    int sock;
    int fd;
    int received;
    int header_len = 0;
    int body_len;
    int total = 0;
    char *buf;
    char *body;
    char request[160];

    if (remote_path[0] != '/')
    {
        rt_kprintf("http_get: remote path must start with /\n");
        return -1;
    }

    buf = rt_malloc(HTTP_GET_BUF_SIZE);
    if (buf == RT_NULL)
    {
        rt_kprintf("http_get: no memory\n");
        return -1;
    }

    fd = open(local_path, O_WRONLY | O_CREAT | O_TRUNC, 0);
    if (fd < 0)
    {
        rt_kprintf("http_get: open %s failed\n", local_path);
        rt_free(buf);
        return -1;
    }

    sock = http_get_connect(host, port);
    if (sock < 0)
    {
        close(fd);
        rt_free(buf);
        return -1;
    }

    rt_snprintf(request, sizeof(request),
                "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
                remote_path, host);
    if (send(sock, request, rt_strlen(request), 0) < 0)
    {
        rt_kprintf("http_get: send request failed\n");
        close(sock);
        close(fd);
        rt_free(buf);
        return -1;
    }

    while (1)
    {
        received = recv(sock, buf + header_len,
                        HTTP_GET_BUF_SIZE - header_len, 0);
        if (received <= 0)
        {
            rt_kprintf("http_get: header recv failed\n");
            close(sock);
            close(fd);
            rt_free(buf);
            return -1;
        }

        header_len += received;
        body = http_get_find_body(buf, header_len);
        if (body != RT_NULL)
        {
            body_len = header_len - (int)(body - buf);
            if (rt_strncmp(buf, "HTTP/1.0 200", 12) != 0 &&
                rt_strncmp(buf, "HTTP/1.1 200", 12) != 0)
            {
                rt_kprintf("http_get: server returned non-200\n");
                close(sock);
                close(fd);
                rt_free(buf);
                return -1;
            }
            if (body_len > 0 && write(fd, body, body_len) != body_len)
            {
                rt_kprintf("http_get: write failed\n");
                close(sock);
                close(fd);
                rt_free(buf);
                return -1;
            }
            total += body_len;
            break;
        }

        if (header_len == HTTP_GET_BUF_SIZE)
        {
            rt_kprintf("http_get: HTTP header too large\n");
            close(sock);
            close(fd);
            rt_free(buf);
            return -1;
        }
    }

    while ((received = recv(sock, buf, HTTP_GET_BUF_SIZE, 0)) > 0)
    {
        if (write(fd, buf, received) != received)
        {
            rt_kprintf("http_get: write failed\n");
            close(sock);
            close(fd);
            rt_free(buf);
            return -1;
        }
        total += received;
        if ((total % (64 * 1024)) < HTTP_GET_BUF_SIZE)
        {
            rt_kprintf("http_get: %d bytes\n", total);
        }
    }

    close(sock);
    close(fd);
    rt_free(buf);
    rt_kprintf("http_get: saved %d bytes to %s\n", total, local_path);

    return total;
}

static int http_get(int argc, char **argv)
{
    int port;

    if (argc != 5)
    {
        rt_kprintf("Usage: http_get <host> <port> <remote_path> <local_path>\n");
        rt_kprintf("eg: http_get 192.168.10.9 8000 /test.bin /test.bin\n");
        return -1;
    }

    port = atoi(argv[2]);
    if (port <= 0 || port > 65535)
    {
        rt_kprintf("http_get: invalid port\n");
        return -1;
    }

    return http_get_save_file(argv[1], port, argv[3], argv[4]) >= 0 ? 0 : -1;
}
MSH_CMD_EXPORT(http_get, download file by HTTP GET);
static MQTTClient mqtt_notify_client;
static rt_bool_t mqtt_notify_started = RT_FALSE;
static rt_thread_t mqtt_heartbeat_thread = RT_NULL;
static rt_uint32_t mqtt_heartbeat_seq = 0;
static rt_uint32_t mqtt_message_seq = 0;
static my_mqtt_pc_status_t mqtt_pc_status = {
    0, 0, 0, 0, -1, "--", RT_FALSE, 0, 0
};

struct mqtt_history_item
{
    char topic[MQTT_TOPIC_SIZE];
    char payload[MQTT_PAYLOAD_SIZE];
};

static struct mqtt_history_item mqtt_history[MQTT_HISTORY_COUNT] = {
    {"<", "terminal ready"}
};
static rt_size_t mqtt_history_used = 1;

static const char *mqtt_topics[] = {
    MQTT_TOPIC_NOTIFY,
    MQTT_TOPIC_PC_STATUS,
    MQTT_TOPIC_TODO,
};

/*
 * 安全复制文本到固定长度缓冲区。
 * 如果文本太长，会截断；截断时尽量避开 UTF-8 中文字符的中间字节，
 * 防止 LVGL 显示半个乱码字符。
 */
static void mqtt_copy_text(char *dst, rt_size_t dst_size,
                           const char *src, rt_size_t src_len)
{
    rt_size_t copy_len = src_len;

    if (dst == RT_NULL || dst_size == 0)
    {
        return;
    }

    if (copy_len >= dst_size)
    {
        copy_len = dst_size - 1;

        while (copy_len > 0 &&
               (((const rt_uint8_t *)src)[copy_len - 1] & 0xC0) == 0x80)
        {
            copy_len--;
        }
    }

    rt_memcpy(dst, src, copy_len);
    dst[copy_len] = '\0';
}

static void mqtt_push_history(const char *topic, rt_size_t topic_len,
                              const char *payload, rt_size_t payload_len)
{
    /*
     * mqtt_history[0] 始终保存最新一条消息。
     * 新消息进来时，旧消息整体往后挪一格，超出 MQTT_HISTORY_COUNT 的丢弃。
     */
    rt_size_t i;

    if (mqtt_history_used < MQTT_HISTORY_COUNT)
    {
        mqtt_history_used++;
    }

    for (i = mqtt_history_used - 1; i > 0; --i)
    {
        mqtt_history[i] = mqtt_history[i - 1];
    }

    mqtt_copy_text(mqtt_history[0].topic, sizeof(mqtt_history[0].topic),
                   topic, topic_len);
    mqtt_copy_text(mqtt_history[0].payload, sizeof(mqtt_history[0].payload),
                   payload, payload_len);
    mqtt_message_seq++;
}

static void mqtt_push_terminal_line(const char *prefix, const char *text)
{
    mqtt_push_history(prefix, rt_strlen(prefix), text, rt_strlen(text));
}

static void mqtt_terminal_output(const char *prefix, const char *text)
{
    rt_kprintf("%s %s\n", prefix, text);
    mqtt_push_terminal_line(prefix, text);
}

static rt_bool_t mqtt_is_heartbeat_payload(const char *payload, rt_size_t len)
{
    /* 心跳消息只用于保活，不需要显示成普通命令历史。 */
    if (payload == RT_NULL)
    {
        return RT_FALSE;
    }

    if (len >= 3 && payload[0] == 'h' && payload[1] == 'b' && payload[2] == '=')
    {
        return RT_TRUE;
    }

    if (len == 9 && rt_strncmp(payload, "heartbeat", 9) == 0)
    {
        return RT_TRUE;
    }

    return RT_FALSE;
}

static rt_bool_t mqtt_ascii_iequals(const char *a, const char *b)
{
    char ca;
    char cb;

    if (a == RT_NULL || b == RT_NULL)
    {
        return RT_FALSE;
    }

    while (*a != '\0' && *b != '\0')
    {
        ca = *a;
        cb = *b;
        if (ca >= 'A' && ca <= 'Z')
        {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z')
        {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb)
        {
            return RT_FALSE;
        }
        a++;
        b++;
    }

    return (*a == '\0' && *b == '\0') ? RT_TRUE : RT_FALSE;
}

static int mqtt_read_int_field(const char *payload, const char *key, int *value)
{
    /*
     * 从形如 "cpu=23,mem=45,down=..." 的状态字符串里读取整数字段。
     * key 只传字段名，例如 "cpu"。
     */
    const char *pos = rt_strstr(payload, key);
    char *endptr;

    if (pos == RT_NULL || value == RT_NULL)
    {
        return -RT_ERROR;
    }

    pos += rt_strlen(key);
    if (*pos != '=')
    {
        return -RT_ERROR;
    }

    pos++;
    *value = strtol(pos, &endptr, 10);
    if (endptr == pos)
    {
        return -RT_ERROR;
    }

    return RT_EOK;
}

static int mqtt_read_text_field(const char *payload, const char *key,
                                char *value, rt_size_t value_size)
{
    /* 读取逗号分隔状态字符串中的文本字段，例如 ip=192.168.1.8。 */
    const char *pos = rt_strstr(payload, key);
    rt_size_t len = 0;

    if (pos == RT_NULL || value == RT_NULL || value_size == 0)
    {
        return -RT_ERROR;
    }

    pos += rt_strlen(key);
    if (*pos != '=')
    {
        return -RT_ERROR;
    }

    pos++;
    while (pos[len] != '\0' && pos[len] != ',' && len < value_size - 1)
    {
        len++;
    }

    if (len == 0)
    {
        return -RT_ERROR;
    }

    mqtt_copy_text(value, value_size, pos, len);

    return RT_EOK;
}

static rt_uint8_t mqtt_percent_limit(int value)
{
    if (value < 0)
    {
        return 0;
    }
    if (value > 100)
    {
        return 100;
    }

    return (rt_uint8_t)value;
}

static void mqtt_handle_pc_status(const char *payload, rt_size_t payload_len)
{
    /*
     * 处理 PC 状态主题 MQTT_TOPIC_PC_STATUS。
     * 解析后的数据不会直接改 LVGL 对象，而是保存到 mqtt_pc_status；
     * ui.c 会在 UI 线程里读取它并刷新屏幕。
     */
    char buf[MQTT_PAYLOAD_SIZE];
    int value;

    mqtt_copy_text(buf, sizeof(buf), payload, payload_len);

    if (mqtt_read_int_field(buf, "cpu", &value) == RT_EOK)
    {
        mqtt_pc_status.cpu = mqtt_percent_limit(value);
    }
    if (mqtt_read_int_field(buf, "mem", &value) == RT_EOK)
    {
        mqtt_pc_status.mem = mqtt_percent_limit(value);
    }
    if (mqtt_read_int_field(buf, "down", &value) == RT_EOK)
    {
        mqtt_pc_status.down = (value < 0) ? 0 : (rt_uint32_t)value;
    }
    if (mqtt_read_int_field(buf, "up", &value) == RT_EOK)
    {
        mqtt_pc_status.up = (value < 0) ? 0 : (rt_uint32_t)value;
    }
    if (mqtt_read_int_field(buf, "bat", &value) == RT_EOK)
    {
        if (value < 0)
        {
            mqtt_pc_status.battery = -1;
        }
        else if (value > 100)
        {
            mqtt_pc_status.battery = 100;
        }
        else
        {
            mqtt_pc_status.battery = (rt_int16_t)value;
        }
    }
    if (mqtt_read_text_field(buf, "ip", mqtt_pc_status.ip,
                             sizeof(mqtt_pc_status.ip)) != RT_EOK &&
        mqtt_pc_status.ip[0] == '\0')
    {
        mqtt_copy_text(mqtt_pc_status.ip, sizeof(mqtt_pc_status.ip),
                       "--", 2);
    }

    mqtt_pc_status.online = RT_TRUE;
    mqtt_pc_status.last_tick_ms = rt_tick_get_millisecond();
    mqtt_pc_status.seq++;
}

static rt_bool_t mqtt_text_equals(const char *data, rt_size_t len, const char *cmd)
{
    return rt_strlen(cmd) == len && rt_strncmp(data, cmd, len) == 0;
}

static char *mqtt_trim_text(char *text)
{
    /* 去掉命令前后的空白，方便后面统一判断命令名和参数。 */
    char *end;

    if (text == RT_NULL)
    {
        return RT_NULL;
    }

    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
    {
        text++;
    }

    end = text + rt_strlen(text);
    while (end > text &&
           (end[-1] == ' ' || end[-1] == '\t' ||
            end[-1] == '\r' || end[-1] == '\n'))
    {
        end--;
    }
    *end = '\0';

    return text;
}

static rt_bool_t mqtt_cmd_is(const char *cmd, const char *name)
{
    return mqtt_ascii_iequals(cmd, name);
}

static rt_bool_t mqtt_cmd_has_arg(const char *cmd, const char *name,
                                  char **arg)
{
    /*
     * 判断命令是否是“命令名 + 参数”的形式。
     * 例如 cmd="switch home"，name="switch"，返回 true，并把 arg 指向 home。
     */
    rt_size_t len = rt_strlen(name);
    rt_size_t i;

    for (i = 0; i < len; ++i)
    {
        char ca = cmd[i];
        char cb = name[i];

        if (ca >= 'A' && ca <= 'Z')
        {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z')
        {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb)
        {
            return RT_FALSE;
        }
    }

    if (cmd[len] != ' ' && cmd[len] != '\t')
    {
        return RT_FALSE;
    }

    if (arg != RT_NULL)
    {
        *arg = mqtt_trim_text((char *)cmd + len);
    }

    return RT_TRUE;
}

static void mqtt_handle_switch_cmd(const char *page_name)
{
    /*
     * 通过 MQTT 远程切换页面。
     * 注意这里调用 app_ui_request_xxx()，只是提交请求；
     * 真正操作 LVGL 对象会在 UI 线程里执行。
     */
    if (mqtt_ascii_iequals(page_name, "message"))
    {
        app_ui_request_page(APP_UI_PAGE_MESSAGE);
        mqtt_terminal_output("<", "switch Message OK");
    }
    else if (mqtt_ascii_iequals(page_name, "home"))
    {
        app_ui_request_page(APP_UI_PAGE_HOME);
        mqtt_terminal_output("<", "switch Home OK");
    }
    else if (mqtt_ascii_iequals(page_name, "options"))
    {
        app_ui_request_active(APP_UI_ACTIVE_OPTIONS);
        app_ui_request_page(APP_UI_PAGE_OPTIONS);
        mqtt_terminal_output("<", "switch Options OK");
    }
    else if (mqtt_ascii_iequals(page_name, "settings"))
    {
        app_ui_request_active(APP_UI_ACTIVE_SETTING);
        app_ui_request_page(APP_UI_PAGE_OPTIONS);
        mqtt_terminal_output("<", "switch Settings OK");
    }
    else if (mqtt_ascii_iequals(page_name, "about"))
    {
        app_ui_request_active(APP_UI_ACTIVE_ABOUT);
        app_ui_request_page(APP_UI_PAGE_OPTIONS);
        mqtt_terminal_output("<", "switch About OK");
    }
    else
    {
        char reply[MQTT_PAYLOAD_SIZE];

        rt_snprintf(reply, sizeof(reply), "unknown page: %s", page_name);
        mqtt_terminal_output("!", reply);
    }
}

static void mqtt_handle_clear_cmd(void)
{
    mqtt_history_used = 0;
    mqtt_message_seq++;
    mqtt_terminal_output("<", "clear OK");
}

static void mqtt_handle_help_cmd(void)
{
    /* 把 MCU 支持的远程命令打印到消息历史里，方便手机/电脑端查看。 */
    mqtt_terminal_output("<", "Page:");
    mqtt_terminal_output("<", "switch message/home");
    mqtt_terminal_output("<", "switch options");
    mqtt_terminal_output("<", "switch settings/about");
    mqtt_terminal_output("<", "Setting:");
    mqtt_terminal_output("<", "setting list");
    mqtt_terminal_output("<", "File:");
    mqtt_terminal_output("<", "ls/pwd/cd/mkdir/rm");
    mqtt_terminal_output("<", "cat/echo/df");
    mqtt_terminal_output("<", "System: ps/free/reboot/clear");
}

static void mqtt_handle_setting_cmd(const char *arg)
{
    if (arg == RT_NULL || arg[0] == '\0' || mqtt_ascii_iequals(arg, "list"))
    {
        mqtt_terminal_output("<", "English UI only");
        return;
    }

    mqtt_terminal_output("!", "unknown setting, try: setting list");
}

static void mqtt_handle_free_cmd(void)
{
    /* 查询 RT-Thread 堆内存使用情况。 */
    rt_uint32_t total;
    rt_uint32_t used;
    rt_uint32_t max_used;
    char line[MQTT_PAYLOAD_SIZE];

    rt_memory_info(&total, &used, &max_used);
    rt_snprintf(line, sizeof(line), "total: %u bytes", total);
    mqtt_terminal_output("<", line);
    rt_snprintf(line, sizeof(line), "used : %u bytes", used);
    mqtt_terminal_output("<", line);
    rt_snprintf(line, sizeof(line), "max  : %u bytes", max_used);
    mqtt_terminal_output("<", line);
}

static void mqtt_handle_pwd_cmd(void)
{
    /* 显示 DFS 当前工作目录。 */
    char cwd[DFS_PATH_MAX];

    if (getcwd(cwd, sizeof(cwd)) == RT_NULL)
    {
        mqtt_terminal_output("!", "pwd failed");
        return;
    }

    mqtt_terminal_output("<", cwd);
}

static void mqtt_handle_cd_cmd(const char *path)
{
    char cwd[DFS_PATH_MAX];
    char line[MQTT_PAYLOAD_SIZE];

    if (path == RT_NULL || path[0] == '\0')
    {
        path = "/";
    }

    if (chdir(path) != 0)
    {
        rt_snprintf(line, sizeof(line), "cd failed: %s", path);
        mqtt_terminal_output("!", line);
        return;
    }

    if (getcwd(cwd, sizeof(cwd)) == RT_NULL)
    {
        mqtt_terminal_output("<", "cd OK");
        return;
    }

    rt_snprintf(line, sizeof(line), "cwd: %s", cwd);
    mqtt_terminal_output("<", line);
}

static void mqtt_handle_mkdir_cmd(const char *path)
{
    char line[MQTT_PAYLOAD_SIZE];

    if (path == RT_NULL || path[0] == '\0')
    {
        mqtt_terminal_output("!", "mkdir: missing path");
        return;
    }

    if (mkdir(path, 0) != 0)
    {
        rt_snprintf(line, sizeof(line), "mkdir failed: %s", path);
        mqtt_terminal_output("!", line);
        return;
    }

    rt_snprintf(line, sizeof(line), "mkdir OK: %s", path);
    mqtt_terminal_output("<", line);
}

static void mqtt_handle_rm_cmd(const char *path)
{
    char line[MQTT_PAYLOAD_SIZE];

    if (path == RT_NULL || path[0] == '\0')
    {
        mqtt_terminal_output("!", "rm: missing path");
        return;
    }

    if (unlink(path) != 0 && rmdir(path) != 0)
    {
        rt_snprintf(line, sizeof(line), "rm failed: %s", path);
        mqtt_terminal_output("!", line);
        return;
    }

    rt_snprintf(line, sizeof(line), "rm OK: %s", path);
    mqtt_terminal_output("<", line);
}

static void mqtt_handle_cat_cmd(const char *path)
{
    int fd;
    int len;
    char buf[64];
    char line[MQTT_PAYLOAD_SIZE];

    if (path == RT_NULL || path[0] == '\0')
    {
        mqtt_terminal_output("!", "cat: missing file");
        return;
    }

    fd = open(path, O_RDONLY, 0);
    if (fd < 0)
    {
        rt_snprintf(line, sizeof(line), "cat: cannot open %s", path);
        mqtt_terminal_output("!", line);
        return;
    }

    while ((len = read(fd, buf, sizeof(buf) - 1)) > 0)
    {
        buf[len] = '\0';
        mqtt_terminal_output("<", buf);
    }

    close(fd);
}

static void mqtt_handle_echo_redirect(const char *text, const char *path,
                                      rt_bool_t append)
{
    int fd;
    int flags;
    int len;
    char line[MQTT_PAYLOAD_SIZE];

    if (path == RT_NULL || path[0] == '\0')
    {
        mqtt_terminal_output("!", "echo: missing file");
        return;
    }

    if (text == RT_NULL)
    {
        text = "";
    }

    flags = O_WRONLY | O_CREAT;
    if (append)
    {
        flags |= O_APPEND;
    }
    else
    {
        flags |= O_TRUNC;
    }

    fd = open(path, flags, 0);
    if (fd < 0)
    {
        rt_snprintf(line, sizeof(line), "echo: cannot open %s", path);
        mqtt_terminal_output("!", line);
        return;
    }

    len = write(fd, text, rt_strlen(text));
    if (len >= 0)
    {
        write(fd, "\n", 1);
    }
    close(fd);

    if (len < 0)
    {
        rt_snprintf(line, sizeof(line), "echo: write failed: %s", path);
        mqtt_terminal_output("!", line);
        return;
    }

    rt_snprintf(line, sizeof(line), "echo %s: %s",
                append ? "append OK" : "write OK", path);
    mqtt_terminal_output("<", line);
}

static void mqtt_handle_echo_cmd(const char *text)
{
    char *redirect;
    rt_bool_t append = RT_FALSE;
    char *path;

    if (text == RT_NULL)
    {
        text = "";
    }

    redirect = rt_strstr(text, ">>");
    if (redirect != RT_NULL)
    {
        append = RT_TRUE;
    }
    else
    {
        redirect = rt_strstr(text, ">");
    }

    if (redirect != RT_NULL)
    {
        *redirect = '\0';
        path = redirect + (append ? 2 : 1);
        mqtt_handle_echo_redirect(mqtt_trim_text((char *)text),
                                  mqtt_trim_text(path),
                                  append);
        return;
    }

    mqtt_terminal_output("<", text);
}

static void mqtt_handle_df_cmd(const char *path)
{
    struct statfs fs;
    char line[MQTT_PAYLOAD_SIZE];
    rt_uint32_t total_kb;
    rt_uint32_t free_kb;

    if (path == RT_NULL || path[0] == '\0')
    {
        path = "/";
    }

    if (statfs(path, &fs) != 0)
    {
        rt_snprintf(line, sizeof(line), "df failed: %s", path);
        mqtt_terminal_output("!", line);
        return;
    }

    total_kb = (rt_uint32_t)((fs.f_bsize * fs.f_blocks) / 1024);
    free_kb = (rt_uint32_t)((fs.f_bsize * fs.f_bfree) / 1024);

    rt_snprintf(line, sizeof(line), "fs: %s", path);
    mqtt_terminal_output("<", line);
    rt_snprintf(line, sizeof(line), "total: %uKB", total_kb);
    mqtt_terminal_output("<", line);
    rt_snprintf(line, sizeof(line), "free : %uKB", free_kb);
    mqtt_terminal_output("<", line);
}

static const char *mqtt_thread_status_text(rt_uint8_t stat)
{
    stat &= RT_THREAD_STAT_MASK;

    if (stat == RT_THREAD_READY)
    {
        return "ready";
    }
    if (stat == RT_THREAD_SUSPEND)
    {
        return "suspend";
    }
    if (stat == RT_THREAD_INIT)
    {
        return "init";
    }
    if (stat == RT_THREAD_CLOSE)
    {
        return "close";
    }
    if (stat == RT_THREAD_RUNNING)
    {
        return "running";
    }

    return "unknown";
}

static void mqtt_handle_ps_cmd(void)
{
    struct rt_object_information *info;
    rt_list_t *node;
    char line[MQTT_PAYLOAD_SIZE];
    rt_size_t shown = 0;

    info = rt_object_get_information(RT_Object_Class_Thread);
    if (info == RT_NULL)
    {
        mqtt_terminal_output("!", "ps failed");
        return;
    }

    mqtt_terminal_output("<", "thread      pri stat");
    for (node = info->object_list.next;
         node != &info->object_list;
         node = node->next)
    {
        struct rt_thread *thread;

        thread = rt_list_entry(node, struct rt_thread, list);
        rt_snprintf(line, sizeof(line), "%-10.*s %3d %s",
                    RT_NAME_MAX, thread->name,
                    thread->current_priority,
                    mqtt_thread_status_text(thread->stat));
        mqtt_terminal_output("<", line);
        shown++;
    }

    if (shown == 0)
    {
        mqtt_terminal_output("<", "(no thread)");
    }
}

static void mqtt_handle_reboot_cmd(void)
{
    mqtt_terminal_output("<", "rebooting...");
    rt_thread_mdelay(100);
    rt_hw_cpu_reset();
}

static void mqtt_handle_ls_cmd(const char *path)
{
    /* 列出 littlefs/DFS 目录内容。 */
    DIR *dir;
    struct dirent *entry;
    char line[MQTT_PAYLOAD_SIZE];
    rt_size_t shown = 0;

    if (path == RT_NULL || path[0] == '\0')
    {
        path = "/";
    }

    dir = opendir(path);
    if (dir == RT_NULL)
    {
        rt_snprintf(line, sizeof(line), "ls: cannot open %s", path);
        mqtt_terminal_output("!", line);
        return;
    }

    rt_snprintf(line, sizeof(line), "ls %s", path);
    mqtt_terminal_output("<", line);

    while ((entry = readdir(dir)) != RT_NULL)
    {
        rt_snprintf(line, sizeof(line), "%c %s",
                    entry->d_type == FT_DIRECTORY ? 'd' : '-',
                    entry->d_name);
        mqtt_terminal_output("<", line);
        shown++;
    }

    closedir(dir);

    if (shown == 0)
    {
        mqtt_terminal_output("<", "(empty)");
    }
}

static rt_bool_t mqtt_handle_command(const char *topic, rt_size_t topic_len,
                                     const char *payload, rt_size_t payload_len)
{
    /*
     * MQTT 命令分发中心。
     *
     * 只有 MQTT_TOPIC_NOTIFY 这个主题会被当作远程命令处理。
     * 支持 switch、ls、pwd、cd、mkdir、rm、cat、echo、df、ps、free、
     * help、clear、reboot 等命令。
     */
    char cmd[MQTT_PAYLOAD_SIZE];
    char *trimmed_cmd;
    char *arg = RT_NULL;

    if (!mqtt_text_equals(topic, topic_len, MQTT_TOPIC_NOTIFY))
    {
        return RT_FALSE;
    }

    mqtt_copy_text(cmd, sizeof(cmd), payload, payload_len);
    trimmed_cmd = mqtt_trim_text(cmd);
    if (trimmed_cmd == RT_NULL || trimmed_cmd[0] == '\0')
    {
        return RT_TRUE;
    }

    mqtt_terminal_output(">", trimmed_cmd);

    if (mqtt_cmd_has_arg(trimmed_cmd, "switch", &arg))
    {
        mqtt_handle_switch_cmd(arg);
        return RT_TRUE;
    }

    if (mqtt_cmd_is(trimmed_cmd, "setting"))
    {
        mqtt_handle_setting_cmd("list");
        return RT_TRUE;
    }

    if (mqtt_cmd_has_arg(trimmed_cmd, "setting", &arg))
    {
        mqtt_handle_setting_cmd(arg);
        return RT_TRUE;
    }


    if (mqtt_cmd_is(trimmed_cmd, "ls"))
    {
        mqtt_handle_ls_cmd(".");
        return RT_TRUE;
    }

    if (mqtt_cmd_has_arg(trimmed_cmd, "ls", &arg))
    {
        mqtt_handle_ls_cmd(arg);
        return RT_TRUE;
    }

    if (mqtt_cmd_is(trimmed_cmd, "pwd"))
    {
        mqtt_handle_pwd_cmd();
        return RT_TRUE;
    }

    if (mqtt_cmd_is(trimmed_cmd, "cd"))
    {
        mqtt_handle_cd_cmd("/");
        return RT_TRUE;
    }

    if (mqtt_cmd_has_arg(trimmed_cmd, "cd", &arg))
    {
        mqtt_handle_cd_cmd(arg);
        return RT_TRUE;
    }

    if (mqtt_cmd_has_arg(trimmed_cmd, "mkdir", &arg))
    {
        mqtt_handle_mkdir_cmd(arg);
        return RT_TRUE;
    }

    if (mqtt_cmd_has_arg(trimmed_cmd, "rm", &arg))
    {
        mqtt_handle_rm_cmd(arg);
        return RT_TRUE;
    }

    if (mqtt_cmd_has_arg(trimmed_cmd, "cat", &arg))
    {
        mqtt_handle_cat_cmd(arg);
        return RT_TRUE;
    }

    if (mqtt_cmd_is(trimmed_cmd, "echo"))
    {
        mqtt_handle_echo_cmd("");
        return RT_TRUE;
    }

    if (mqtt_cmd_has_arg(trimmed_cmd, "echo", &arg))
    {
        mqtt_handle_echo_cmd(arg);
        return RT_TRUE;
    }

    if (mqtt_cmd_is(trimmed_cmd, "df"))
    {
        mqtt_handle_df_cmd("/");
        return RT_TRUE;
    }

    if (mqtt_cmd_has_arg(trimmed_cmd, "df", &arg))
    {
        mqtt_handle_df_cmd(arg);
        return RT_TRUE;
    }

    if (mqtt_cmd_is(trimmed_cmd, "ps"))
    {
        mqtt_handle_ps_cmd();
        return RT_TRUE;
    }

    if (mqtt_cmd_is(trimmed_cmd, "free"))
    {
        mqtt_handle_free_cmd();
        return RT_TRUE;
    }

    if (mqtt_cmd_is(trimmed_cmd, "help"))
    {
        mqtt_handle_help_cmd();
        return RT_TRUE;
    }

    if (mqtt_cmd_is(trimmed_cmd, "clear"))
    {
        mqtt_handle_clear_cmd();
        return RT_TRUE;
    }

    if (mqtt_cmd_is(trimmed_cmd, "reboot"))
    {
        mqtt_handle_reboot_cmd();
        return RT_TRUE;
    }

    mqtt_terminal_output("!", "unknown cmd, try: help");
    return RT_TRUE;
}

void my_mqtt_get_history_text(char *buf, rt_size_t size)
{
    /*
     * 旧版 UI 用这个接口一次性取格式化后的历史文本。
     * 当前 ui.c 主要使用 my_mqtt_get_history_item() 分条显示。
     */
    rt_size_t i;
    rt_size_t used = 0;

    if (buf == RT_NULL || size == 0)
    {
        return;
    }

    buf[0] = '\0';

    for (i = mqtt_history_used; i > 0; --i)
    {
        rt_size_t index = i - 1;
        int written = rt_snprintf(buf + used, size - used,
                                  "#%s %s%s:#\n#%s %s#%s",
                                  (index == 0) ? "ff9900" : "808080",
                                  mqtt_history[index].topic,
                                  (index == 0) ? "(new)" : "",
                                  (index == 0) ? "ff9900" : "808080",
                                  mqtt_history[index].payload,
                                  (i > 1) ? "\n\n" : "");

        if (written < 0)
        {
            break;
        }

        if ((rt_size_t)written >= size - used)
        {
            buf[size - 1] = '\0';
            break;
        }

        used += (rt_size_t)written;
    }
}

rt_uint32_t my_mqtt_get_message_seq(void)
{
    return mqtt_message_seq;
}

rt_size_t my_mqtt_get_history_count(void)
{
    return mqtt_history_used;
}

rt_bool_t my_mqtt_get_history_item(rt_size_t display_index,
                                   char *topic, rt_size_t topic_size,
                                   char *payload, rt_size_t payload_size,
                                   rt_bool_t *is_new)
{
    rt_size_t index;

    if (display_index >= mqtt_history_used)
    {
        return RT_FALSE;
    }

    index = display_index;
    mqtt_copy_text(topic, topic_size, mqtt_history[index].topic,
                   rt_strlen(mqtt_history[index].topic));
    mqtt_copy_text(payload, payload_size, mqtt_history[index].payload,
                   rt_strlen(mqtt_history[index].payload));

    if (is_new != RT_NULL)
    {
        *is_new = (index == 0) ? RT_TRUE : RT_FALSE;
    }

    return RT_TRUE;
}

void my_mqtt_get_pc_status(my_mqtt_pc_status_t *status)
{
    if (status == RT_NULL)
    {
        return;
    }

    *status = mqtt_pc_status;
}

static void mqtt_heartbeat_entry(void *parameter)
{
    /*
     * 周期发布 MCU 心跳，证明设备在线。
     * 收到自己的心跳时，mqtt_is_heartbeat_payload() 会过滤掉，
     * 避免心跳刷满消息页。
     */
    char payload[48];

    (void)parameter;

    while (mqtt_notify_started)
    {
        rt_thread_mdelay(MQTT_HEARTBEAT_MS);

        if (!mqtt_notify_started || !mqtt_notify_client.isconnected)
        {
            continue;
        }

        mqtt_heartbeat_seq++;
        rt_snprintf(payload, sizeof(payload), "hb=%u,tick=%u",
                    mqtt_heartbeat_seq,
                    (rt_uint32_t)rt_tick_get_millisecond());

        if (paho_mqtt_publish(&mqtt_notify_client, QOS1,
                              MQTT_TOPIC_NOTIFY, payload) != RT_EOK)
        {
            rt_kprintf("[mqtt] heartbeat publish failed\n");
        }
    }

    mqtt_heartbeat_thread = RT_NULL;
}

static void mqtt_start_heartbeat(void)
{
    if (mqtt_heartbeat_thread != RT_NULL)
    {
        return;
    }

    mqtt_heartbeat_thread = rt_thread_create("mqtt_hb",
                                             mqtt_heartbeat_entry,
                                             RT_NULL,
                                             MQTT_HEARTBEAT_STACK,
                                             MQTT_HEARTBEAT_PRIO,
                                             10);
    if (mqtt_heartbeat_thread == RT_NULL)
    {
        rt_kprintf("[mqtt] heartbeat thread create failed\n");
        return;
    }

    rt_thread_startup(mqtt_heartbeat_thread);
}

static void mqtt_notify_message_cb(MQTTClient *client, MessageData *msg_data)
{
    /*
     * MQTT 收包回调。
     * 不同主题走不同处理：
     * - PC 状态主题：更新 mqtt_pc_status。
     * - 命令主题：尝试当作远程命令执行。
     * - 其它普通消息：压入消息历史，给 UI 消息页显示。
     */
    rt_bool_t is_pc_status;
    rt_bool_t is_notify;
    rt_bool_t is_heartbeat;

    (void)client;

    is_pc_status = mqtt_text_equals(msg_data->topicName->lenstring.data,
                                    msg_data->topicName->lenstring.len,
                                    MQTT_TOPIC_PC_STATUS);
    is_notify = mqtt_text_equals(msg_data->topicName->lenstring.data,
                                 msg_data->topicName->lenstring.len,
                                 MQTT_TOPIC_NOTIFY);
    is_heartbeat = is_notify &&
                   mqtt_is_heartbeat_payload((const char *)msg_data->message->payload,
                                             msg_data->message->payloadlen);
    if (is_pc_status)
    {
        mqtt_handle_pc_status((const char *)msg_data->message->payload,
                              msg_data->message->payloadlen);
    }

    if (!is_pc_status && !is_heartbeat &&
        !mqtt_handle_command(msg_data->topicName->lenstring.data,
                             msg_data->topicName->lenstring.len,
                             (const char *)msg_data->message->payload,
                             msg_data->message->payloadlen))
    {
        char text[MQTT_PAYLOAD_SIZE];
        rt_size_t used;

        mqtt_copy_text(text, sizeof(text),
                       msg_data->topicName->lenstring.data,
                       msg_data->topicName->lenstring.len);
        used = rt_strlen(text);
        if (used < sizeof(text) - 2)
        {
            text[used++] = ':';
            text[used++] = ' ';
            text[used] = '\0';
        }
        mqtt_copy_text(text + used, sizeof(text) - used,
                       (const char *)msg_data->message->payload,
                       msg_data->message->payloadlen);
        mqtt_push_terminal_line("<", text);
    }

    rt_kprintf("[mqtt] topic: %.*s\n",
               msg_data->topicName->lenstring.len,
               msg_data->topicName->lenstring.data);
    rt_kprintf("[mqtt] payload: %.*s\n",
               msg_data->message->payloadlen,
               (char *)msg_data->message->payload);
}

static void mqtt_notify_online_cb(MQTTClient *client)
{
    (void)client;
    rt_kprintf("[mqtt] online, subscribed: %s, %s, %s\n",
               MQTT_TOPIC_NOTIFY, MQTT_TOPIC_PC_STATUS, MQTT_TOPIC_TODO);
}

static void mqtt_notify_offline_cb(MQTTClient *client)
{
    (void)client;
    rt_kprintf("[mqtt] offline\n");
}

int my_mqtt_start(void)
{
    /*
     * MQTT 启动入口：
     * 1. 配置服务器 URI、client id、keepalive。
     * 2. 分配 Paho MQTT 收发缓冲区。
     * 3. 注册订阅主题对应的 message callback。
     * 4. 调用 paho_mqtt_start() 连接并订阅。
     */
    int result;
    rt_size_t i;
    MQTTPacket_connectData condata = MQTTPacket_connectData_initializer;

    if (mqtt_notify_started)
    {
        rt_kprintf("[mqtt] already started\n");
        return 0;
    }

    rt_memset(&mqtt_notify_client, 0, sizeof(mqtt_notify_client));

    mqtt_notify_client.uri = MQTT_NOTIFY_URI;
    rt_memcpy(&mqtt_notify_client.condata, &condata, sizeof(condata));
    mqtt_notify_client.condata.clientID.cstring = MQTT_NOTIFY_CLIENTID;
    mqtt_notify_client.condata.keepAliveInterval = 30;
    mqtt_notify_client.condata.cleansession = 1;

    mqtt_notify_client.buf_size = 1024;
    mqtt_notify_client.readbuf_size = 1024;
    mqtt_notify_client.buf = rt_calloc(1, mqtt_notify_client.buf_size);
    mqtt_notify_client.readbuf = rt_calloc(1, mqtt_notify_client.readbuf_size);
    if (mqtt_notify_client.buf == RT_NULL || mqtt_notify_client.readbuf == RT_NULL)
    {
        rt_kprintf("[mqtt] no memory for buffers\n");
        if (mqtt_notify_client.buf)
        {
            rt_free(mqtt_notify_client.buf);
        }
        if (mqtt_notify_client.readbuf)
        {
            rt_free(mqtt_notify_client.readbuf);
        }
        return -RT_ENOMEM;
    }

    mqtt_notify_client.online_callback = mqtt_notify_online_cb;
    mqtt_notify_client.offline_callback = mqtt_notify_offline_cb;
    for (i = 0; i < sizeof(mqtt_topics) / sizeof(mqtt_topics[0]); ++i)
    {
        mqtt_notify_client.messageHandlers[i].topicFilter = rt_strdup(mqtt_topics[i]);
        mqtt_notify_client.messageHandlers[i].callback = mqtt_notify_message_cb;
        mqtt_notify_client.messageHandlers[i].qos = QOS1;
        if (mqtt_notify_client.messageHandlers[i].topicFilter == RT_NULL)
        {
            rt_kprintf("[mqtt] no memory for topic\n");
            while (i > 0)
            {
                i--;
                rt_free(mqtt_notify_client.messageHandlers[i].topicFilter);
            }
            rt_free(mqtt_notify_client.buf);
            rt_free(mqtt_notify_client.readbuf);
            rt_memset(&mqtt_notify_client, 0, sizeof(mqtt_notify_client));
            return -RT_ENOMEM;
        }
    }

    rt_kprintf("[mqtt] connecting %s\n", MQTT_NOTIFY_URI);
    result = paho_mqtt_start(&mqtt_notify_client);
    if (result != RT_EOK)
    {
        rt_kprintf("[mqtt] start failed: %d\n", result);
        rt_free(mqtt_notify_client.buf);
        rt_free(mqtt_notify_client.readbuf);
        for (i = 0; i < sizeof(mqtt_topics) / sizeof(mqtt_topics[0]); ++i)
        {
            rt_free(mqtt_notify_client.messageHandlers[i].topicFilter);
        }
        rt_memset(&mqtt_notify_client, 0, sizeof(mqtt_notify_client));
        return result;
    }

    mqtt_notify_started = RT_TRUE;
    mqtt_start_heartbeat();

    return 0;
}

static void mqtt_join_args(char *buf, rt_size_t size, int argc, char **argv, int start)
{
    /* 把 MSH 命令的多个参数重新拼成一整段文本。 */
    int i;
    rt_size_t used = 0;

    if (buf == RT_NULL || size == 0)
    {
        return;
    }
    buf[0] = '\0';

    for (i = start; i < argc; ++i)
    {
        int written;

        if (used >= size - 1)
        {
            break;
        }
        written = rt_snprintf(buf + used, size - used, "%s%s",
                              used == 0 ? "" : " ", argv[i]);
        if (written <= 0)
        {
            break;
        }
        used += (rt_size_t)written;
    }
}

static int setting(int argc, char **argv)
{
    /* 本地 MSH 入口，等价于远程 MQTT 的 setting 命令。 */
    char arg[MQTT_PAYLOAD_SIZE];

    if (argc <= 1)
    {
        mqtt_handle_setting_cmd("list");
        return 0;
    }

    mqtt_join_args(arg, sizeof(arg), argc, argv, 1);
    mqtt_handle_setting_cmd(arg);
    return 0;
}
MSH_CMD_EXPORT(setting, UI setting command);

static int switch_cmd(int argc, char **argv)
{
    /* 本地 MSH 入口，等价于远程 MQTT 的 switch 命令。 */
    if (argc != 2)
    {
        rt_kprintf("Usage: switch <message|home|options|settings|about>\n");
        return -1;
    }

    mqtt_handle_switch_cmd(argv[1]);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(switch_cmd, switch, UI switch command);
static int mqtt_start(int argc, char **argv)
{
    /* 本地 MSH 命令：手动启动 MQTT。main.c 里也会自动启动一次。 */
    (void)argc;
    (void)argv;

    return my_mqtt_start();
}
MSH_CMD_EXPORT(mqtt_start, start mqtt subscribe);

static int mqtt_stop(int argc, char **argv)
{
    /* 本地 MSH 命令：停止 MQTT 客户端，主要用于调试。 */
    int result;

    (void)argc;
    (void)argv;

    if (!mqtt_notify_started)
    {
        rt_kprintf("[mqtt] not started\n");
        return 0;
    }

    result = paho_mqtt_stop(&mqtt_notify_client);
    mqtt_notify_started = RT_FALSE;

    rt_kprintf("[mqtt] stop %s\n", result == RT_EOK ? "OK" : "failed");

    return result;
}
MSH_CMD_EXPORT(mqtt_stop, stop mqtt subscribe);










