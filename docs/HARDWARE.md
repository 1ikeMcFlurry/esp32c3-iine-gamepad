# 硬件与烧录

[返回首页](../README.md)

## 已测硬件

- ESP32-C3，8 MB Flash，原生 USB Serial/JTAG。
- 240 × 320 ST7789P3 / T200H7-C14-05 LCD，RGB565。
- GPIO0 ADC1 电阻分压三键：上、下、确定。
- IINE 口袋手柄 Pro L1241，私有身份 `0710 / 0220 / 27.02`。

这是从工牌项目提取的独立固件。硬件默认值位于 [`main/gamepad_screen.c`](../main/gamepad_screen.c)，移植时先检查这些配置。

## 引脚

| 信号 | ESP32-C3 GPIO | 配置 |
| --- | --- | --- |
| LCD MOSI | 9 | SPI2 |
| LCD SCLK | 8 | 默认 80 MHz；不同布线需验证 |
| LCD CS | 1 | 片选 |
| LCD DC | 20 | 数据 / 命令 |
| LCD 背光 | 21 | LEDC，5 kHz、10 bit，首帧后 80% |
| LCD RESET | 未由 GPIO 控制 | `reset_gpio_num = -1`；需要板上正确复位电路 |
| LCD MISO | 未使用 | 单向显示 |
| ADC 按键 | 0 | ADC1 channel 0，12 dB 衰减 |
| USB D− / D+ | 18 / 19 | ESP32-C3 原生 USB |

供电电压、背光驱动及上电电路必须符合所用模块规格；不要把 GPIO 直接当作裸背光的电源。GPIO8/9 同时涉及芯片启动配置，外接模块不能破坏原板的启动电平。具体电气要求以 [ESP32-C3 官方数据手册](https://www.espressif.com/sites/default/files/documentation/esp32-c3_datasheet_en.pdf) 为准。

默认 ADC 分类（校准后的 mV）：

| 电压 | 按键 |
| --- | --- |
| `< 150` | 上 |
| `150–446` | 下 |
| `447–1899` | 确定 |
| `≥ 1900` | 松开 |

40 ms 去抖；确定键长按阈值 900 ms；上电时已按住的键需先松开。不同电阻网络必须重新设置阈值。

## 烧录与分区

应用构建目标为 **ESP32-C3 / 8 MB Flash**，自定义表见 [`partitions.csv`](../partitions.csv)。

### 已有工牌：仅更新应用

已测设备实际使用游戏分区表：`game_app` 在 `0x10000`，容量 `0x300000`；`roms` 在 `0x310000`；`saves` 在 `0x710000`。在确认其应用与 NVS 位置兼容后，仅更新了应用；原分区表保留。现场升级前后校验了 bootloader、分区表、ROM、存档未变化。

`app-flash` 使用**本地构建的应用地址**，不会自动读取并验证设备的现有分区。使用前须确认：

1. 现有 bootloader 可以启动 `0x10000` 的 factory 应用。
2. 此处应用分区足够容纳构建镜像；本项目预留 3 MB。
3. NVS 位于 `0x9000`、大小 `0x6000`，与本项目兼容。
4. 已了解并保留设备上其他数据区；需要时先做备份。

```sh
idf.py -p COM3 app-flash monitor
```

这会替换现有应用；不要求擦除整片 Flash。NVS 用于保存配对密钥和历史设备，正常运行会更新这些记录；初始化失败时停止，不自动格式化整个 NVS。

### 空白板：完整烧录

只对接线匹配、允许使用本项目分区的空白 8 MB 设备执行：

```sh
idf.py -p COM3 flash monitor
```

完整烧录会写 bootloader、分区表和应用。仓库的分区表保留工牌 `imgstore`、`imgframe`、`cardid`、`audio`、`imgava` 布局，但仓库不附带这些数据镜像，demo 也不需要它们。

它与上面实测设备的游戏分区表不同。不要对需要保留 ROM / 存档的已有设备直接完整烧录，也不要把此配置原样用于 4 MB 板。

## 显示与内存

屏幕由独立任务绘制，每条 16 行、7,680 字节 DMA 缓冲，不分配整屏 framebuffer。颜色使用 RGB565，传输前转换字节序；默认开启面板反色，无镜像和偏移。

字体包含固定中文 UI 字符与 ASCII 子集，四个字号。列表中未包含的字符显示 `?`。公开版本使用 Noto Sans CJK SC 点阵子集，普通编译无需安装字体。
