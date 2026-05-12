#include "Lcd.h"
#include "Lcd_Reg.h"
#include <rtthread.h>
#include <rtdevice.h>

/*
 * 保存“当前窗口”内总共有多少个像素点。
 * LCD_Set_Window() 会更新这个值，
 * LCD_Fill_Color() 则会根据这个值连续写入颜色数据。
 */
static uint32_t lcd_window_pixels = (uint32_t)LCD_WIDTH * (uint32_t)LCD_HEIGHT;
static struct rt_spi_device *lcd_spi_dev = RT_NULL;
static uint8_t lcd_tx_burst[1024];

static struct rt_spi_device *LCD_GetSpiDevice(void);
static HAL_StatusTypeDef LCD_WriteBytesChecked(const uint8_t *data, uint32_t size);

/* 拉低 CS，表示选中 LCD 设备 */
static void LCD_Select(void)
{
    HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_RESET);
}

/* 拉高 CS，表示结束本次 LCD 访问 */
static void LCD_Unselect(void)
{
    HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
}

/* DC=0，表示接下来发送的是“命令” */
static void LCD_DC_CommandMode(void)
{
    HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_RESET);
}

/* DC=1，表示接下来发送的是“数据” */
static void LCD_DC_DataMode(void)
{
    HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_SET);
}

/*
 * 通过 HAL SPI 向 LCD 连续发送一段字节流。
 * 这里底层固定使用 hspi2，
 * 优先保证流程正确，超时先使用 HAL_MAX_DELAY。
 */
static struct rt_spi_device *LCD_GetSpiDevice(void)
{
    /*
     * "lcd" 这个名字来自 board_devcie.c 的 rt_hw_spi_device_attach()。
     * 第一次使用时查找一次，后续缓存指针，避免每次写像素都查设备表。
     */
    if (lcd_spi_dev == RT_NULL) {
        lcd_spi_dev = (struct rt_spi_device *)rt_device_find("lcd");
    }

    return lcd_spi_dev;
}

static void LCD_WriteBytes(const uint8_t *data, uint16_t size)
{
    (void)LCD_WriteBytesChecked(data, size);
}

static HAL_StatusTypeDef LCD_WriteBytesChecked(const uint8_t *data, uint32_t size)
{
    struct rt_spi_device *spi_dev;
    uint32_t offset = 0U;

    if ((data == NULL) || (size == 0U)) {
        return HAL_OK;
    }

    spi_dev = LCD_GetSpiDevice();
    if (spi_dev == RT_NULL) {
        rt_kprintf("lcd spi device not found\n");
        return HAL_ERROR;
    }

    while (offset < size) {
        /*
         * 大块像素数据拆成小块发送，避免一次 rt_spi_send() 传太大。
         * 这仍然是阻塞式发送：函数返回时当前 chunk 已经发完。
         */
        rt_size_t chunk = (size - offset > 4096U) ? 4096U : (rt_size_t)(size - offset);

        if (rt_spi_send(spi_dev, data + offset, chunk) != chunk) {
            return HAL_ERROR;
        }

        offset += chunk;
    }

    return HAL_OK;
}

/*
 * 写“1 条命令 + 1 字节参数”的辅助函数。
 * 适合像像素格式、Gamma 选择这类简单寄存器写入。
 */
static void LCD_WriteRegister8(uint8_t reg, uint8_t value)
{
    LCD_WriteCommand(reg);
    LCD_WriteData8(value);
}

/*
 * 在一次片选周期内，先发送命令，再发送对应的数据区。
 * 这种写法更贴近 LCD 初始化表，也更紧凑。
 */
static void LCD_WriteCommandBuffer(uint8_t cmd, const uint8_t *data, uint16_t size)
{
    LCD_Select();
    LCD_DC_CommandMode();
    LCD_WriteBytes(&cmd, 1U);
    if ((data != NULL) && (size > 0U)) {
        LCD_DC_DataMode();
        LCD_WriteBytes(data, size);
    }
    LCD_Unselect();
}

void LCD_WriteCommand(uint8_t cmd)
{
    /* 单独发送 1 个命令字节 */
    LCD_Select();
    LCD_DC_CommandMode();
    LCD_WriteBytes(&cmd, 1U);
    LCD_Unselect();
}

void LCD_WriteData8(uint8_t data)
{
    /* 单独发送 1 个 8 位数据 */
    LCD_Select();
    LCD_DC_DataMode();
    LCD_WriteBytes(&data, 1U);
    LCD_Unselect();
}

void LCD_WriteData16(uint16_t data)
{
    uint8_t buf[2];

    /*
     * ILI9341 写 16 位数据时通常按高字节在前的顺序发送。
     * 例如 RGB565 红色 0xF800，会先发 0xF8，再发 0x00。
     */
    buf[0] = (uint8_t)(data >> 8);
    buf[1] = (uint8_t)(data & 0xFFU);

    LCD_Select();
    LCD_DC_DataMode();
    LCD_WriteBytes(buf, 2U);
    LCD_Unselect();
}

void LCD_Set_Window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    uint8_t data[4];

    /*
     * 坐标合法性检查：
     * 1. 左上角不能跑到右下角后面
     * 2. 右下角不能超出屏幕边界
     */
    if ((x1 > x2) || (y1 > y2) || (x2 >= LCD_WIDTH) || (y2 >= LCD_HEIGHT)) {
        return;
    }

    /*
     * 记录当前窗口内的像素总数，
     * 后续 LCD_Fill_Color() 会按这个数量持续灌颜色。
     */
    lcd_window_pixels = ((uint32_t)x2 - (uint32_t)x1 + 1U) * ((uint32_t)y2 - (uint32_t)y1 + 1U);

    /*
     * CASET(0x2A)：设置列地址范围，也就是 X 方向窗口。
     * 数据格式：
     * x_start_high, x_start_low, x_end_high, x_end_low
     */
    data[0] = (uint8_t)(x1 >> 8);
    data[1] = (uint8_t)(x1 & 0xFFU);
    data[2] = (uint8_t)(x2 >> 8);
    data[3] = (uint8_t)(x2 & 0xFFU);
    LCD_WriteCommand(ILI9341_CASET);
    LCD_Select();
    LCD_DC_DataMode();
    LCD_WriteBytes(data, 4U);
    LCD_Unselect();

    /*
     * PASET(0x2B)：设置页地址范围，也就是 Y 方向窗口。
     * 数据格式：
     * y_start_high, y_start_low, y_end_high, y_end_low
     */
    data[0] = (uint8_t)(y1 >> 8);
    data[1] = (uint8_t)(y1 & 0xFFU);
    data[2] = (uint8_t)(y2 >> 8);
    data[3] = (uint8_t)(y2 & 0xFFU);
    LCD_WriteCommand(ILI9341_PASET);
    LCD_Select();
    LCD_DC_DataMode();
    LCD_WriteBytes(data, 4U);
    LCD_Unselect();

    /*
     * RAMWR(0x2C)：设置完窗口后，告诉 LCD
     * “接下来通过 SPI 发来的就是像素数据”。
     */
    LCD_WriteCommand(ILI9341_RAMWR);
}

void LCD_Fill_Color(uint16_t color)
{
    uint32_t remaining;
    uint32_t i;

    /*
     * 先构造一小段重复颜色缓存。
     * 每 2 个字节表示 1 个 RGB565 像素，
     * 这样比每个像素都单独组织数据更高效。
     */
    for (i = 0; i < (sizeof(lcd_tx_burst) / 2U); ++i) {
        lcd_tx_burst[2U * i] = (uint8_t)(color >> 8);
        lcd_tx_burst[2U * i + 1U] = (uint8_t)(color & 0xFFU);
    }

    /*
     * 向当前窗口持续写入同一种颜色。
     * 这里不重新设置窗口，默认窗口已经由 LCD_Set_Window() 配好。
     */
    remaining = lcd_window_pixels;
    while (remaining > 0U) {
        uint16_t chunk_pixels;

        chunk_pixels = (remaining > (sizeof(lcd_tx_burst) / 2U)) ? (uint16_t)(sizeof(lcd_tx_burst) / 2U) : (uint16_t)remaining;
        if (LCD_WritePixels(lcd_tx_burst, (uint32_t)chunk_pixels * 2U) != HAL_OK) {
            Error_Handler();
        }
        remaining -= chunk_pixels;
    }
}

HAL_StatusTypeDef LCD_WritePixels(const uint8_t *data, uint32_t size)
{
    HAL_StatusTypeDef status;

    if ((data == NULL) || (size == 0U)) {
        return HAL_OK;
    }

    LCD_Select();
    LCD_DC_DataMode();
    /* 像素数据属于 LCD 数据模式，所以 DC 必须为 1。 */
    status = LCD_WriteBytesChecked(data, size);
    LCD_Unselect();

    return status;
}

void LCD_Init(void)
{
    static const uint8_t power_b[] = {0x00U, 0xD9U, 0x30U};
    static const uint8_t power_seq[] = {0x64U, 0x03U, 0x12U, 0x81U};
    static const uint8_t driver_timing_a[] = {0x85U, 0x10U, 0x78U};
    static const uint8_t power_a[] = {0x39U, 0x2CU, 0x00U, 0x34U, 0x02U};
    static const uint8_t prc[] = {0x20U};
    static const uint8_t dtcb[] = {0x00U, 0x00U};
    static const uint8_t vmctr1[] = {0x32U, 0x3CU};
    static const uint8_t frmctr1[] = {0x00U, 0x18U};
    static const uint8_t dfunctr[] = {0x0AU, 0xA2U};
    static const uint8_t gamma_pos[] = {
        0x0FU, 0x20U, 0x1EU, 0x09U, 0x12U, 0x0BU, 0x50U, 0xBAU,
        0x44U, 0x09U, 0x14U, 0x05U, 0x23U, 0x21U, 0x00U
    };
    static const uint8_t gamma_neg[] = {
        0x00U, 0x19U, 0x19U, 0x00U, 0x12U, 0x07U, 0x2DU, 0x28U,
        0x3FU, 0x02U, 0x0AU, 0x08U, 0x25U, 0x2DU, 0x0FU
    };

    /*
     * 这里是一整套 ILI9341 初始化序列。
     * 你可以把它理解成“把屏幕控制器从上电默认状态，配置成本工程需要的
     * RGB565、竖屏、正常显示模式”。
     */
    /*
     * 先把控制脚拉到稳定状态：
     * 1. CS 拉高，避免误选中 LCD
     * 2. DC 拉高，默认数据态
     * 3. BLK 拉高，打开背光，便于观察是否点亮
     */
    HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(LCD_DC_GPIO_Port, LCD_DC_Pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(LCD_BLK_GPIO_Port, LCD_BLK_Pin, GPIO_PIN_SET);

    /* 给 LCD 模组一点上电稳定时间 */
    HAL_Delay(20U);

    /*
     * 软件复位 LCD 控制器，让内部状态回到已知起点。
     * 这一步能减少偶发的上电初始化不一致问题。
     */
    LCD_WriteCommand(ILI9341_SWRESET);
    HAL_Delay(50U);

    /*
     * 下面这组初始化参数参考了仓库里的大越例程。
     * 这些寄存器主要用于配置：
     * 1. 电源时序
     * 2. 驱动时序
     * 3. VCOM
     * 4. 帧率
     * 5. Gamma 曲线
     *
     * 这部分通常和具体屏模适配关系很强，
     * 所以这里尽量向已知可工作的例程靠拢。
     */
    LCD_WriteCommandBuffer(ILI9341_POWERB, power_b, sizeof(power_b));
    LCD_WriteCommandBuffer(ILI9341_POWER_SEQ, power_seq, sizeof(power_seq));
    LCD_WriteCommandBuffer(ILI9341_DTCA, driver_timing_a, sizeof(driver_timing_a));
    LCD_WriteCommandBuffer(ILI9341_POWERA, power_a, sizeof(power_a));
    LCD_WriteCommandBuffer(ILI9341_PRC, prc, sizeof(prc));
    LCD_WriteCommandBuffer(ILI9341_DTCB, dtcb, sizeof(dtcb));

    LCD_WriteRegister8(ILI9341_PWCTR1, 0x21U);
    LCD_WriteRegister8(ILI9341_PWCTR2, 0x12U);
    LCD_WriteCommandBuffer(ILI9341_VMCTR1, vmctr1, sizeof(vmctr1));
    LCD_WriteRegister8(ILI9341_VMCTR2, 0xC1U);
    LCD_WriteRegister8(ILI9341_MADCTL, ILI9341_MADCTL_BGR);
    LCD_WriteRegister8(ILI9341_PIXFMT, 0x55U);
    LCD_WriteCommandBuffer(ILI9341_FRMCTR1, frmctr1, sizeof(frmctr1));
    LCD_WriteCommandBuffer(ILI9341_DFUNCTR, dfunctr, sizeof(dfunctr));
    LCD_WriteRegister8(ILI9341_ENABLE3G, 0x00U);
    LCD_WriteRegister8(ILI9341_GAMSET, 0x01U);
    LCD_WriteCommandBuffer(ILI9341_GMCTRP1, gamma_pos, sizeof(gamma_pos));
    LCD_WriteCommandBuffer(ILI9341_GMCTRN1, gamma_neg, sizeof(gamma_neg));

    /*
     * 退出休眠，然后开启正常显示。
     * Exit Sleep 后必须给足延时，否则有些屏会不稳定。
     */
    LCD_WriteCommand(ILI9341_SLPOUT);
    HAL_Delay(90U);
    LCD_WriteCommand(ILI9341_INVOFF);
    LCD_WriteCommand(ILI9341_NORON);
    HAL_Delay(10U);
    LCD_WriteCommand(ILI9341_DISPON);
    HAL_Delay(10U);

    /*
     * 默认把绘图窗口设置成整屏，
     * 这样初始化完成后就可以直接做整屏刷色测试。
     */
    LCD_Set_Window(0U, 0U, LCD_WIDTH - 1U, LCD_HEIGHT - 1U);
}
