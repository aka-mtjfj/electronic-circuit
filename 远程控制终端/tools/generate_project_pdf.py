import html
import os
import re
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
APP_DIR = ROOT / "applications"
DOC_DIR = ROOT / "docs"
HTML_OUT = DOC_DIR / "rt_lvgl_littlefs_project_explained.html"
PDF_OUT = DOC_DIR / "rt_lvgl_littlefs_project_explained.pdf"


APP_FILES = [
    "SConscript",
    "main.c",
    "board_devcie.c",
    "Lcd.h",
    "Lcd_Reg.h",
    "Lcd.c",
    "lv_port_disp.h",
    "lv_port_disp.c",
    "ui.h",
    "ui.c",
    "my_mqtt.h",
    "my_mqtt.c",
    "net_test.c",
    "ui_config.c",
    "avatar_img.c",
    "ui_font_zh_14.c",
]


FILE_PURPOSE = {
    "SConscript": "RT-Thread Studio/SCons 构建入口，把 applications 下的 C 文件组成 Applications 分组并加入编译。",
    "main.c": "应用主入口。负责 LCD 初始化、LVGL 初始化、创建 UI、启动 UI 刷新线程和 MQTT 线程。",
    "board_devcie.c": "板级设备注册层。负责 SPI1/SPI2 引脚复用、LCD SPI 设备挂载、W25Q32 SPI/SFUD/MTD/littlefs 挂载、ESP8266 AT 客户端初始化。",
    "Lcd.h": "LCD 驱动对外接口头文件，定义屏幕尺寸、控制脚和像素写入接口。",
    "Lcd_Reg.h": "ILI9341 控制器寄存器/命令常量，避免在驱动里直接散落魔法数字。",
    "Lcd.c": "ILI9341 SPI LCD 底层驱动。负责片选、命令/数据切换、设置绘图窗口、写像素和初始化寄存器。",
    "lv_port_disp.h": "LVGL 显示端口对外接口，给 main.c 调用显示驱动注册函数。",
    "lv_port_disp.c": "LVGL 到 LCD 的桥。实现 flush_cb，把 LVGL 算好的像素区域写到 ILI9341。",
    "ui.h": "应用 UI 模块接口和页面枚举，给 MQTT 命令层切换页面使用。",
    "ui.c": "LVGL 页面实现。创建消息页、主页、选项页，并根据 MQTT 数据刷新状态、网络、时间、消息历史。",
    "my_mqtt.h": "MQTT 模块对外接口，定义 PC 状态结构和启动/读取状态函数。",
    "my_mqtt.c": "MQTT、远程命令和文件系统小终端。连接巴法云 MQTT，订阅主题，解析 PC 状态和命令，调用 UI 切页，访问 littlefs。",
    "net_test.c": "MSH 网络测试命令，提供 udp_test 发送 UDP 数据包验证网络链路。",
    "ui_config.c": "兼容保留文件。语言切换移除后仍保留，避免旧工程文件列表引用它时构建失败。",
    "avatar_img.c": "LVGL 图片资源，把头像位图转换成 C 数组和 lv_img_dsc_t 描述符。",
    "ui_font_zh_14.c": "LVGL 中文字体资源，把字形、unicode 映射和字体描述符编译进固件。",
}


COMPONENTS = [
    ("RT-Thread 内核", "线程、tick、延时、日志、自动初始化、设备对象、MSH 命令。代码里常见 rt_thread_create、rt_thread_mdelay、rt_tick_get_millisecond、INIT_DEVICE_EXPORT、MSH_CMD_EXPORT。"),
    ("RT-Thread SPI 设备框架", "把硬件 SPI 总线和具体片选设备抽象成 rt_spi_device。LCD 使用 spi2/lcd，W25Q32 使用 spi1/w25q32。"),
    ("STM32 HAL", "提供 GPIO、SPI MSP 初始化、延时和错误状态类型。LCD 驱动里用 HAL_GPIO_WritePin 控制 CS/DC/BLK。"),
    ("ILI9341 LCD 控制器", "240x320 RGB565 显示屏控制芯片。驱动通过 CASET/PASET/RAMWR 设置窗口并写入像素。"),
    ("LVGL 8.3.11", "嵌入式 GUI。main.c 调 lv_init，lv_port_disp.c 注册显示驱动，ui.c 创建标签、图片和对象。"),
    ("SFUD + MTD NOR", "SFUD 识别和读写 SPI Flash，MTD NOR 把 Flash 包装成 RT-Thread 块设备，给 littlefs 使用。"),
    ("littlefs + DFS/POSIX", "littlefs 提供掉电友好的小型文件系统，DFS/POSIX 让代码能用 open/read/write/opendir/statfs 等接口。"),
    ("SAL/AT/ESP8266", "SAL 统一 socket API，AT 设备包通过 ESP8266 提供网络能力，MQTT 和 UDP 测试都依赖网络栈。"),
    ("Paho MQTT", "提供 MQTTClient、paho_mqtt_start、paho_mqtt_publish、messageHandlers 等接口，用于云端消息和远程命令。"),
    ("MSH/FinSH", "命令行外壳。工程导出 mqtt_start、mqtt_stop、udp_test、http_get、switch 等命令便于调试。"),
]


DOMAIN_TERMS = {
    "LCD_Init": "初始化 ILI9341 屏幕控制器，让屏幕进入可显示状态。",
    "LCD_Set_Window": "告诉 LCD 接下来要写哪一块矩形区域。",
    "LCD_WritePixels": "把 RGB565 像素字节流通过 SPI 写进当前 LCD 窗口。",
    "lv_init": "初始化 LVGL 核心库。",
    "lv_timer_handler": "驱动 LVGL 定时器、动画和刷新流程；必须周期性调用。",
    "lv_disp_flush_ready": "通知 LVGL 本次 flush 已经完成，可以继续下一块刷新。",
    "lv_disp_drv_register": "向 LVGL 注册实际显示设备。",
    "app_ui_create": "创建应用 UI 的所有页面和控件。",
    "app_ui_update": "周期性刷新 UI 状态、消息、时间和心跳。",
    "my_mqtt_start": "启动 MQTT 客户端并订阅主题。",
    "paho_mqtt_start": "启动 Paho MQTT 线程/连接流程。",
    "paho_mqtt_publish": "向 MQTT 主题发布消息。",
    "rt_hw_spi_device_attach": "把 SPI 总线和片选 GPIO 组合成一个 RT-Thread SPI 设备。",
    "rt_spi_configure": "配置 SPI 数据宽度、模式和速度。",
    "rt_spi_send": "通过 RT-Thread SPI 设备发送字节流。",
    "rt_sfud_flash_probe": "探测 SPI Flash 并注册为 SFUD flash 设备。",
    "rt_mtd_nor_register_device": "把 NOR Flash 包装成 MTD 设备。",
    "dfs_mount": "把文件系统挂载到指定路径。",
    "dfs_mkfs": "格式化文件系统。",
    "at_client_init": "初始化 AT 客户端，绑定到串口。",
    "MSH_CMD_EXPORT": "把 C 函数导出为 RT-Thread MSH 命令。",
    "INIT_DEVICE_EXPORT": "把初始化函数放入设备初始化阶段自动执行。",
    "INIT_APP_EXPORT": "把初始化函数放入应用初始化阶段自动执行。",
}


def read_text(path):
    data = path.read_bytes()
    for enc in ("utf-8-sig", "utf-8", "gb18030", "latin1"):
        try:
            return data.decode(enc)
        except UnicodeDecodeError:
            pass
    return data.decode("utf-8", errors="replace")


def esc(text):
    return html.escape(text, quote=False)


def extract_defines(text, prefixes):
    rows = []
    for line in text.splitlines():
        m = re.match(r"\s*#define\s+([A-Za-z0-9_]+)(?:\s+(.*))?$", line)
        if not m:
            continue
        name = m.group(1)
        if any(name.startswith(p) for p in prefixes):
            rows.append((name, (m.group(2) or "").strip()))
    return rows


def explain_define(name, value):
    if name.startswith("RT_USING_"):
        return "RT-Thread 功能开关：启用对应内核、驱动或组件能力。"
    if name.startswith("PKG_USING_"):
        return "RT-Thread 软件包开关：把对应第三方/软件包加入工程。"
    if name.startswith("LV_USE_"):
        return "LVGL 功能裁剪开关：1 表示启用，0 表示关闭以节省 Flash/RAM。"
    if name.startswith("LV_COLOR"):
        return "LVGL 颜色配置，影响像素格式和屏幕输出字节顺序。"
    if name.startswith("LFS_"):
        return "littlefs 参数，决定读写粒度、块大小、缓存和线程安全行为。"
    if "STACK" in name:
        return "线程栈大小配置，栈越大越不容易溢出，但会占更多 RAM。"
    if "PRIORITY" in name or "PRIO" in name:
        return "线程优先级配置；RT-Thread 数字越小优先级越高。"
    return "宏定义，用名字替代固定值，方便统一修改和表达含义。"


def annotate_line(line, state):
    stripped = line.strip()
    file_name = state["file"]
    current = state.get("function")

    if stripped == "":
        return "空行，用来分隔逻辑块，让代码更容易阅读。"
    if stripped.startswith("/*"):
        state["block_comment"] = True
        if "*/" in stripped:
            state["block_comment"] = False
        return "块注释开始/内容，解释接下来代码为什么这样写。"
    if state.get("block_comment"):
        if "*/" in stripped:
            state["block_comment"] = False
            return "块注释结束。"
        return "块注释内容，属于说明文字，不会被编译进程序逻辑。"
    if stripped.startswith("//"):
        return "单行注释，用来提示这一行附近代码的意图。"
    if stripped.startswith("#include"):
        target = stripped.replace("#include", "").strip()
        return f"引入头文件 {target}，让当前文件可以使用其中声明的类型、宏和函数。"
    if stripped.startswith("#ifdef") or stripped.startswith("#if"):
        return "条件编译开始：只有配置宏满足时，下面这段代码才会参与编译。"
    if stripped.startswith("#elif") or stripped.startswith("#else"):
        return "条件编译分支：当前一组配置条件的另一个选择。"
    if stripped.startswith("#endif"):
        return "条件编译结束。"
    m = re.match(r"#define\s+([A-Za-z0-9_]+)(?:\s+(.*))?", stripped)
    if m:
        return explain_define(m.group(1), m.group(2) or "")
    if stripped.startswith("#ifndef"):
        return "头文件保护开始，避免同一个头文件被重复包含导致重复定义。"
    if stripped.startswith("#pragma"):
        return "编译器指示语，控制编译器的特殊行为。"
    if stripped in ("{", "};", "}", "};"):
        if stripped == "{":
            return f"代码块开始，进入 {current or '当前结构'} 的内部逻辑。"
        if stripped == "}":
            return f"代码块结束，离开 {current or '当前结构'} 的内部逻辑。"
        return "结构体、枚举或初始化表结束。"

    func_match = re.match(r"(static\s+)?([A-Za-z_][\w\s\*]+?)\s+([A-Za-z_]\w*)\s*\((.*)\)\s*(\{|;)?$", stripped)
    if func_match and not stripped.startswith(("if", "for", "while", "switch")):
        name = func_match.group(3)
        if func_match.group(5) == "{":
            state["function"] = name
            return f"定义函数 {name}，从这里开始实现它的具体逻辑。"
        return f"声明函数 {name}，告诉编译器后面会有这个接口。"

    for term, meaning in DOMAIN_TERMS.items():
        if term in stripped:
            return meaning

    if stripped.startswith("typedef enum"):
        return "定义枚举类型，用一组有名字的常量表达状态或选项。"
    if stripped.startswith("typedef struct") or stripped.startswith("struct "):
        return "定义结构体类型，把一组相关数据字段放在一起。"
    if stripped.startswith("static "):
        if "=" in stripped:
            return "定义当前文件私有的静态变量或函数，并给出初始值；外部文件不能直接访问。"
        return "定义当前文件私有的静态对象或函数，限制作用域，降低模块耦合。"
    if stripped.startswith("extern "):
        return "声明外部符号：它的定义在别的文件里，当前文件只是使用它。"
    if stripped.startswith("return"):
        return "从当前函数返回结果；错误码或状态值会交给调用者判断。"
    if stripped.startswith("if " ) or stripped.startswith("if("):
        return "条件判断：只有条件成立时才执行后面的分支，常用于参数检查、错误处理或状态切换。"
    if stripped.startswith("else if"):
        return "条件判断的下一个分支，用来处理另一种情况。"
    if stripped.startswith("else"):
        return "条件判断的兜底分支，前面的条件都不满足时执行。"
    if stripped.startswith("for " ) or stripped.startswith("for("):
        return "for 循环，按固定次数或范围重复执行，用于遍历数组、列表或填充缓冲区。"
    if stripped.startswith("while " ) or stripped.startswith("while("):
        return "while 循环，只要条件成立就持续执行；线程主循环通常写成 while (1)。"
    if stripped.startswith("switch"):
        return "多分支选择，根据一个变量的值进入不同处理路径。"
    if stripped.startswith("case "):
        return "switch 的一个具体分支。"
    if stripped.startswith("break"):
        return "跳出当前循环或 switch 分支。"
    if stripped.startswith("continue"):
        return "跳过本轮循环剩余语句，直接进入下一轮。"
    if stripped.startswith("(void)"):
        return "显式标记参数未使用，避免编译器产生未使用参数警告。"
    if "=" in stripped and stripped.endswith(";"):
        return "赋值语句：更新变量、结构体字段或缓冲区内容，为后续逻辑准备数据。"
    if stripped.endswith(";") and "(" in stripped and ")" in stripped:
        return "函数调用语句：执行一个已有接口，可能完成硬件操作、对象创建、字符串处理或状态更新。"
    if stripped.endswith(",") or stripped.endswith("};"):
        return "初始化表中的一项，用于配置数组、结构体或常量表。"
    if file_name in ("avatar_img.c", "ui_font_zh_14.c"):
        return "资源数据行：把图片或字体的二进制内容以 C 数组形式编译进固件。"
    return "普通 C 语句或声明，服务于当前函数/模块的局部逻辑。"


def source_table(file_name, text):
    state = {"file": file_name, "block_comment": False, "function": None}
    rows = []
    for idx, line in enumerate(text.splitlines(), 1):
        explanation = annotate_line(line, state)
        rows.append(
            "<tr>"
            f"<td class='lineno'>{idx}</td>"
            f"<td class='code'><pre>{esc(line)}</pre></td>"
            f"<td class='explain'>{esc(explanation)}</td>"
            "</tr>"
        )
    return "\n".join(rows)


def render_html():
    rtconfig = read_text(ROOT / "rtconfig.h")
    lvconf = read_text(ROOT / "lv_conf.h")
    rt_rows = extract_defines(rtconfig, ("RT_USING_", "PKG_USING_", "AT_", "SAL_", "NETDEV_", "LFS_", "RTC_", "NTP_", "PAHOMQTT", "MQTT_"))
    lv_rows = extract_defines(lvconf, ("LV_",))

    file_sections = []
    for name in APP_FILES:
        path = APP_DIR / name
        text = read_text(path)
        line_count = len(text.splitlines())
        file_sections.append(
            f"<section class='file-section'>"
            f"<h2>{esc(name)}</h2>"
            f"<p class='purpose'>{esc(FILE_PURPOSE[name])}</p>"
            f"<p>本文件共 {line_count} 行。下面的表格左侧是原始源码，中间是行号，右侧是逐行说明。资源数组文件的数组数据较长，说明会重点解释其作为图片/字体资源的角色。</p>"
            f"<table class='source'><thead><tr><th>行</th><th>源码</th><th>说明</th></tr></thead><tbody>{source_table(name, text)}</tbody></table>"
            f"</section>"
        )

    component_items = "\n".join(f"<tr><td>{esc(a)}</td><td>{esc(b)}</td></tr>" for a, b in COMPONENTS)
    rt_items = "\n".join(f"<tr><td>{esc(a)}</td><td>{esc(b)}</td><td>{esc(explain_define(a, b))}</td></tr>" for a, b in rt_rows)
    lv_items = "\n".join(f"<tr><td>{esc(a)}</td><td>{esc(b)}</td><td>{esc(explain_define(a, b))}</td></tr>" for a, b in lv_rows if a in {
        "LV_COLOR_DEPTH", "LV_COLOR_16_SWAP", "LV_MEM_SIZE", "LV_TICK_CUSTOM",
        "LV_DRAW_COMPLEX", "LV_USE_IMG", "LV_USE_LABEL", "LV_USE_FS_POSIX",
        "LV_USE_THEME_DEFAULT", "LV_FONT_MONTSERRAT_14", "LV_TXT_ENC"
    })

    return f"""<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<title>rt_lvgl_littlefs 工程说明与逐行代码分析</title>
<style>
@page {{ size: A4; margin: 16mm 12mm; }}
body {{ font-family: "Microsoft YaHei", "SimSun", Arial, sans-serif; color: #111; line-height: 1.55; font-size: 12px; }}
h1 {{ font-size: 28px; margin: 0 0 12px; }}
h2 {{ font-size: 20px; border-bottom: 2px solid #222; padding-bottom: 4px; margin-top: 28px; page-break-after: avoid; }}
h3 {{ font-size: 15px; margin-top: 18px; }}
p {{ margin: 6px 0; }}
.cover {{ page-break-after: always; padding-top: 30mm; }}
.subtitle {{ font-size: 15px; color: #444; }}
.note {{ background: #fff7d6; border-left: 4px solid #c48a00; padding: 8px 10px; }}
.flow {{ white-space: pre-wrap; font-family: Consolas, "Microsoft YaHei", monospace; background: #f3f4f6; padding: 10px; border: 1px solid #ddd; }}
table {{ border-collapse: collapse; width: 100%; margin: 8px 0 16px; }}
th, td {{ border: 1px solid #d0d0d0; padding: 5px 6px; vertical-align: top; }}
th {{ background: #ededed; }}
.source {{ table-layout: fixed; font-size: 10px; }}
.source th:nth-child(1), .source td:nth-child(1) {{ width: 34px; }}
.source th:nth-child(2), .source td:nth-child(2) {{ width: 58%; }}
.source th:nth-child(3), .source td:nth-child(3) {{ width: auto; }}
.lineno {{ color: #666; text-align: right; }}
.code pre {{ margin: 0; white-space: pre-wrap; overflow-wrap: anywhere; font-family: Consolas, "Courier New", monospace; font-size: 9px; line-height: 1.35; }}
.explain {{ font-size: 10px; }}
.purpose {{ font-weight: bold; }}
.toc li {{ margin: 3px 0; }}
.file-section {{ page-break-before: always; }}
</style>
</head>
<body>
<section class="cover">
<h1>rt_lvgl_littlefs 工程说明与逐行代码分析</h1>
<p class="subtitle">面向刚开始接触本工程的同学：从整体流程、组件职责、依赖关系，到 applications 目录每个文件的逐行说明。</p>
<p>生成路径：{esc(str(PDF_OUT))}</p>
<p>源码根目录：{esc(str(ROOT))}</p>
<p class="note">提示：源码中部分中文注释/字符串当前呈现为乱码形态，本文档会保留源码原样并从代码行为角度解释其作用。若后续要修复显示文字，可优先检查源文件编码、LVGL 字体字库和字符串编码是否统一为 UTF-8。</p>
</section>

<h2>目录</h2>
<ol class="toc">
<li>工程整体目标</li>
<li>启动与运行流程</li>
<li>使用的组件、原因、接口和依赖</li>
<li>关键配置开关</li>
<li>applications 每个文件逐行说明</li>
</ol>

<h2>工程整体目标</h2>
<p>这个工程把 STM32F407 板子做成一个小型联网显示终端：底层用 RT-Thread 管理线程、设备和文件系统；屏幕使用 ILI9341 SPI LCD；界面用 LVGL 绘制；外部 W25Q32 SPI Flash 通过 SFUD、MTD、littlefs 挂载成文件系统；网络通过 ESP8266 AT 设备接入；云端消息用 Paho MQTT 接收，既能显示 PC 状态，也能通过 MQTT 下发命令切换页面、查看文件系统、执行简单终端命令。</p>

<h2>启动与运行流程</h2>
<pre class="flow">上电
  -> RT-Thread 内核启动
  -> 自动初始化阶段
       -> board_devcie.c: lcd_spi_hw_init() 注册 spi2/lcd
       -> board_devcie.c: w25q32_spi_hw_init() 注册 spi1/w25q32，探测 SFUD，注册 MTD
       -> board_devcie.c: esp8266_at_client_init() 初始化 uart2 AT 客户端
       -> board_devcie.c: w25q32_lfs_mount() 挂载 littlefs 到 /
  -> main()
       -> LCD_Init() 初始化 ILI9341
       -> lv_init() 初始化 LVGL
       -> lv_port_disp_init() 注册 LVGL 显示驱动
       -> app_ui_create() 创建页面和控件
       -> 创建 ui 线程：周期调用 lv_timer_handler() 和 app_ui_update()
       -> 创建 mqtt 线程：延时后 my_mqtt_start()
  -> MQTT 收到消息
       -> my_mqtt.c 回调解析 topic/payload
       -> PC 状态更新到 mqtt_pc_status，UI 下一轮刷新显示
       -> 命令类消息调用 app_ui_request_page()/文件系统/POSIX 接口</pre>

<h2>使用的组件、原因、接口和依赖</h2>
<table><thead><tr><th>组件</th><th>为什么使用 / 提供什么接口 / 依赖关系</th></tr></thead><tbody>{component_items}</tbody></table>

<h2>关键 RT-Thread / 软件包配置</h2>
<table><thead><tr><th>宏</th><th>值</th><th>说明</th></tr></thead><tbody>{rt_items}</tbody></table>

<h2>关键 LVGL 配置</h2>
<table><thead><tr><th>宏</th><th>值</th><th>说明</th></tr></thead><tbody>{lv_items}</tbody></table>

<h2>applications 逐文件逐行说明</h2>
{''.join(file_sections)}
</body>
</html>"""


def find_browser():
    candidates = [
        Path(os.environ.get("ProgramFiles(x86)", "")) / "Microsoft" / "Edge" / "Application" / "msedge.exe",
        Path(os.environ.get("ProgramFiles", "")) / "Microsoft" / "Edge" / "Application" / "msedge.exe",
        Path(os.environ.get("ProgramFiles", "")) / "Google" / "Chrome" / "Application" / "chrome.exe",
        Path(os.environ.get("ProgramFiles(x86)", "")) / "Google" / "Chrome" / "Application" / "chrome.exe",
    ]
    for path in candidates:
        if path.exists():
            return path
    return None


def main():
    DOC_DIR.mkdir(exist_ok=True)
    HTML_OUT.write_text(render_html(), encoding="utf-8")

    browser = find_browser()
    if browser is None:
        raise SystemExit(f"HTML generated, but no Edge/Chrome executable was found: {HTML_OUT}")

    subprocess.run(
        [
            str(browser),
            "--headless=new",
            "--disable-gpu",
            f"--print-to-pdf={PDF_OUT}",
            str(HTML_OUT),
        ],
        check=True,
    )
    print(f"HTML: {HTML_OUT}")
    print(f"PDF : {PDF_OUT}")


if __name__ == "__main__":
    main()
