#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include "drv_spi.h"
#include <rtconfig.h>
#ifdef RT_USING_SFUD
#include "spi_flash_sfud.h"
#endif
#if defined(RT_USING_SFUD) && defined(RT_USING_MTD_NOR)
#include <drivers/mtd_nor.h>
#endif
#if defined(PKG_USING_LITTLEFS) && defined(RT_USING_DFS)
#include <dfs_fs.h>
#endif
#if defined(RT_USING_SFUD) && defined(FINSH_USING_MSH)
#include <finsh.h>
#endif

#define LCD_CS_PIN   GET_PIN(B, 12)
#define LCD_DC_PIN   GET_PIN(C, 5)
#define LCD_BLK_PIN  GET_PIN(B, 1)
#define W25Q32_CS_PIN GET_PIN(A, 15)

/*
 * 这个文件是“板级设备注册胶水层”。
 *
 * 它不负责画 UI，也不负责解析 MQTT；它负责把硬件挂到 RT-Thread
 * 的设备框架里，让上层可以通过名字找到设备：
 * - "lcd"       : ILI9341 屏幕使用的 SPI2 设备
 * - "w25q32"    : 外部 SPI Flash 使用的 SPI1 设备
 * - "flash0"    : SFUD 识别后的 Flash 设备名
 * - "w25q32_mtd": 给 littlefs/DFS 使用的 MTD 块设备
 */
static struct rt_spi_device *lcd_spi_dev = RT_NULL;
static struct rt_spi_device *w25q32_spi_dev = RT_NULL;

#if defined(RT_USING_SFUD) && defined(RT_USING_MTD_NOR)
static sfud_flash_t w25q32_sfud = RT_NULL;
static struct rt_mtd_nor_device w25q32_mtd;

/*
 * MTD 层需要一组 read/write/erase 回调。
 * 这里实际读写仍然交给 SFUD，因为 SFUD 已经处理了 W25Q32 的
 * SPI 命令、页编程、擦除和忙等待等细节。
 */
static rt_err_t w25q32_mtd_read_id(struct rt_mtd_nor_device *device)
{
    (void)device;
    return RT_EOK;
}

static rt_size_t w25q32_mtd_read(struct rt_mtd_nor_device *device,
                                 rt_off_t offset,
                                 rt_uint8_t *data,
                                 rt_uint32_t length)
{
    (void)device;

    if ((w25q32_sfud == RT_NULL) || (offset < 0))
    {
        return 0;
    }

    if (sfud_read(w25q32_sfud, offset, length, data) != SFUD_SUCCESS)
    {
        return 0;
    }

    return length;
}

static rt_size_t w25q32_mtd_write(struct rt_mtd_nor_device *device,
                                  rt_off_t offset,
                                  const rt_uint8_t *data,
                                  rt_uint32_t length)
{
    (void)device;

    if ((w25q32_sfud == RT_NULL) || (offset < 0))
    {
        return 0;
    }

    if (sfud_write(w25q32_sfud, offset, length, data) != SFUD_SUCCESS)
    {
        return 0;
    }

    return length;
}

static rt_err_t w25q32_mtd_erase_block(struct rt_mtd_nor_device *device,
                                       rt_off_t offset,
                                       rt_uint32_t length)
{
    (void)device;

    if ((w25q32_sfud == RT_NULL) || (offset < 0))
    {
        return -RT_ERROR;
    }

    if (sfud_erase(w25q32_sfud, offset, length) != SFUD_SUCCESS)
    {
        return -RT_ERROR;
    }

    return RT_EOK;
}

static const struct rt_mtd_nor_driver_ops w25q32_mtd_ops =
{
    w25q32_mtd_read_id,
    w25q32_mtd_read,
    w25q32_mtd_write,
    w25q32_mtd_erase_block,
};
#endif

void HAL_SPI_MspInit(SPI_HandleTypeDef *hspi)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    if (hspi->Instance == SPI2)
    {
        __HAL_RCC_SPI2_CLK_ENABLE();
        __HAL_RCC_GPIOB_CLK_ENABLE();

        /* SPI2: PB13=SCK, PB14=MISO, PB15=MOSI */
        GPIO_InitStruct.Pin = GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
        GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull = GPIO_NOPULL;
        GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF5_SPI2;
        HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
    }
    else if (hspi->Instance == SPI1)
    {
        __HAL_RCC_SPI1_CLK_ENABLE();
        __HAL_RCC_GPIOB_CLK_ENABLE();

        /* SPI1: PB3=SCK, PB4=MISO, PB5=MOSI */
        GPIO_InitStruct.Pin = GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5;
        GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull = GPIO_NOPULL;
        GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF5_SPI1;
        HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
    }
}

/*
 * 注册 LCD 用的 SPI 设备。
 *
 * 关键点：
 * - 底层总线名是 "spi2"。
 * - 挂上去的新设备名是 "lcd"。
 * - Lcd.c 后面会通过 rt_device_find("lcd") 找到它。
 * - LCD 使用 ILI9341 常见的 SPI Mode 3。
 */
static int lcd_spi_hw_init(void)
{
    rt_err_t ret;
    struct rt_spi_configuration cfg;

    /* LCD control pins. CS is initialized by rt_hw_spi_device_attach(). */
    rt_pin_mode(LCD_DC_PIN, PIN_MODE_OUTPUT);
    rt_pin_mode(LCD_BLK_PIN, PIN_MODE_OUTPUT);

    rt_pin_write(LCD_DC_PIN, PIN_HIGH);
    rt_pin_write(LCD_BLK_PIN, PIN_HIGH);

    ret = rt_hw_spi_device_attach("spi2", "lcd", GPIOB, GPIO_PIN_12);
    if (ret != RT_EOK)
    {
        rt_kprintf("attach lcd failed: %d\n", ret);
        return ret;
    }

    lcd_spi_dev = (struct rt_spi_device *)rt_device_find("lcd");
    if (lcd_spi_dev == RT_NULL)
    {
        rt_kprintf("find lcd failed\n");
        return -RT_ERROR;
    }

    cfg.data_width = 8;
    cfg.mode = RT_SPI_MASTER | RT_SPI_MODE_3 | RT_SPI_MSB;
    cfg.max_hz = 42000000;

    ret = rt_spi_configure(lcd_spi_dev, &cfg);
    if (ret != RT_EOK)
    {
        rt_kprintf("config lcd spi failed: %d\n", ret);
        return ret;
    }

    rt_kprintf("lcd spi init ok\n");
    return RT_EOK;
}
INIT_DEVICE_EXPORT(lcd_spi_hw_init);

/*
 * 注册 W25Q32 外部 Flash。
 *
 * 当 RT_USING_SFUD 打开时，会继续让 SFUD 探测 Flash；
 * 当 RT_USING_MTD_NOR 也打开时，再把 SFUD 设备包装成 MTD NOR，
 * 后续 littlefs 才能通过 DFS 挂载到文件系统。
 */
static int w25q32_spi_hw_init(void)
{
    rt_err_t ret;
    struct rt_spi_configuration cfg;

    ret = rt_hw_spi_device_attach("spi1", "w25q32", GPIOA, GPIO_PIN_15);
    if (ret != RT_EOK)
    {
        rt_kprintf("attach w25q32 failed: %d\n", ret);
        return ret;
    }

    w25q32_spi_dev = (struct rt_spi_device *)rt_device_find("w25q32");
    if (w25q32_spi_dev == RT_NULL)
    {
        rt_kprintf("find w25q32 failed\n");
        return -RT_ERROR;
    }

    cfg.data_width = 8;
    cfg.mode = RT_SPI_MASTER | RT_SPI_MODE_0 | RT_SPI_MSB;
    cfg.max_hz = 20000000;

    ret = rt_spi_configure(w25q32_spi_dev, &cfg);
    if (ret != RT_EOK)
    {
        rt_kprintf("config w25q32 spi failed: %d\n", ret);
        return ret;
    }

#ifdef RT_USING_SFUD
    if (rt_sfud_flash_probe("flash0", "w25q32") == RT_NULL)
    {
        rt_kprintf("sfud probe w25q32 failed\n");
        return -RT_ERROR;
    }

    rt_kprintf("sfud flash0 init ok\n");

#ifdef RT_USING_MTD_NOR
    w25q32_sfud = rt_sfud_flash_find_by_dev_name("flash0");
    if (w25q32_sfud == RT_NULL)
    {
        rt_kprintf("find sfud flash0 failed\n");
        return -RT_ERROR;
    }

    w25q32_mtd.block_size = w25q32_sfud->chip.erase_gran;
    w25q32_mtd.block_start = 0;
    w25q32_mtd.block_end = w25q32_sfud->chip.capacity / w25q32_mtd.block_size;
    w25q32_mtd.ops = &w25q32_mtd_ops;

    ret = rt_mtd_nor_register_device("w25q32_mtd", &w25q32_mtd);
    if (ret != RT_EOK)
    {
        rt_kprintf("register w25q32_mtd failed: %d\n", ret);
        return ret;
    }

    rt_kprintf("w25q32_mtd init ok\n");
#endif
#endif

    rt_kprintf("w25q32 spi init ok\n");
    return RT_EOK;
}
INIT_DEVICE_EXPORT(w25q32_spi_hw_init);

#if defined(PKG_USING_LITTLEFS) && defined(RT_USING_DFS)
/*
 * 把 W25Q32 上的 littlefs 挂载为根目录 "/"。
 *
 * 第一次使用或者 Flash 内容无效时，mount 会失败；
 * 这里会尝试 dfs_mkfs() 格式化，再重新 mount。
 */
static int w25q32_lfs_mount(void)
{
    if (dfs_mount("w25q32_mtd", "/", "lfs", 0, RT_NULL) == 0)
    {
        rt_kprintf("littlefs mount ok\n");
        return RT_EOK;
    }

    rt_kprintf("littlefs mount failed, try mkfs\n");

    if (dfs_mkfs("lfs", "w25q32_mtd") != 0)
    {
        rt_kprintf("littlefs mkfs failed\n");
        return -RT_ERROR;
    }

    if (dfs_mount("w25q32_mtd", "/", "lfs", 0, RT_NULL) != 0)
    {
        rt_kprintf("littlefs mount after mkfs failed\n");
        return -RT_ERROR;
    }

    rt_kprintf("littlefs mkfs and mount ok\n");
    return RT_EOK;
}
INIT_APP_EXPORT(w25q32_lfs_mount);
#endif

#if defined(RT_USING_SFUD) && defined(FINSH_USING_MSH)
/*
 * MSH 测试命令：
 * 在终端输入 w25q32_test，会擦写外部 Flash 最后一个扇区并校验。
 *
 * 这个命令适合在 bring-up 阶段确认：
 * 1. SPI1 连接没错。
 * 2. SFUD 能识别芯片。
 * 3. 擦除、写入、读取链路都正常。
 */
static void w25q32_test(void)
{
    sfud_flash_t flash;
    rt_uint32_t test_addr;
    rt_uint32_t i;
    static rt_uint8_t write_buf[256];
    static rt_uint8_t read_buf[256];

    flash = rt_sfud_flash_find_by_dev_name("flash0");
    if (flash == RT_NULL)
    {
        rt_kprintf("flash0 not found\n");
        return;
    }

    test_addr = flash->chip.capacity - flash->chip.erase_gran;
    rt_kprintf("w25q32 test addr: 0x%08x, size: %d bytes\n", test_addr, sizeof(write_buf));

    for (i = 0; i < sizeof(write_buf); i++)
    {
        write_buf[i] = (rt_uint8_t)(i ^ 0x5a);
        read_buf[i] = 0;
    }

    if (sfud_erase(flash, test_addr, flash->chip.erase_gran) != SFUD_SUCCESS)
    {
        rt_kprintf("w25q32 erase failed\n");
        return;
    }

    if (sfud_write(flash, test_addr, sizeof(write_buf), write_buf) != SFUD_SUCCESS)
    {
        rt_kprintf("w25q32 write failed\n");
        return;
    }

    if (sfud_read(flash, test_addr, sizeof(read_buf), read_buf) != SFUD_SUCCESS)
    {
        rt_kprintf("w25q32 read failed\n");
        return;
    }

    if (rt_memcmp(write_buf, read_buf, sizeof(write_buf)) != 0)
    {
        rt_kprintf("w25q32 verify failed\n");
        return;
    }

    rt_kprintf("w25q32 erase/write/read test ok\n");
}
MSH_CMD_EXPORT(w25q32_test, erase/write/read last sector of W25Q32);
#endif

#ifdef AT_USING_CLIENT
#include <at.h>

/*
 * 初始化 ESP8266 AT 客户端。
 * 这里假定 ESP8266 连接在 uart2，上层 SAL/MQTT 会走这个 AT 网络链路。
 */
static int esp8266_at_client_init(void)
{
    return at_client_init("uart2", 512);
}
INIT_APP_EXPORT(esp8266_at_client_init);
#endif



