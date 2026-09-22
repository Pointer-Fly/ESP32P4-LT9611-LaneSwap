# ESP32-P4 Nano + LT9611 LaneSwap HDMI Demo

这是基于 Espressif `esp-bsp` display 示例整理的可编译工程，已经在以下硬件上完成亮屏验证：

- Waveshare ESP32-P4-Nano，ESP32-P4 revision v1.3
- 普通版 LT9611（不是引脚不兼容的 LT9611UXC）
- HDMI 输出：1280×720@60 Hz，RGB888
- MIPI DSI：2 data lanes，960 Mbit/s/lane
- ESP-IDF：v6.0.2

工程保留了完整 `esp-bsp` 源码层级和 display 示例实际使用的托管组件，因此不依赖原服务器上的旧构建目录或绝对路径。

## 接线

| ESP32-P4 Nano | LT9611 | 说明 |
| --- | --- | --- |
| GPIO7 | SDA | I2C 地址 `0x3b` |
| GPIO8 | SCL | I2C |
| GPIO23 | RESET | 低电平复位 |
| DSI CLK P/N | Port-B CLK P/N | 极性不反转 |
| DSI D0 P/N | Port-B D2 N/P | lane 映射并反转 P/N |
| DSI D1 P/N | Port-B D3 P/N | lane 映射，极性不反转 |

驱动采用已实测的 K230 Port-B 映射组合：

```text
8250=14  8300=62  8303=4f  8304=00  8307=40
824f=80  8302=08  8306=08  830a=00
```

## 编译

先安装并启用 ESP-IDF v6.0.2，然后执行：

```sh
. "$IDF_PATH/export.sh"
cd examples/display

idf.py -B build_lt9611 \
  -D SDKCONFIG=sdkconfig.lt9611 \
  -D SDKCONFIG_DEFAULTS=sdkconfig.bsp.waveshare_p4_nano_lt9611 \
  build
```

在当前 espserver 的标准安装路径上，也可以这样启用环境：

```sh
. /root/.espressif/v6.0.2/esp-idf/export.sh
```

## 烧录与串口监视

P4 USB 串口为 `/dev/ttyACM0` 时：

```sh
idf.py -B build_lt9611 -p /dev/ttyACM0 flash monitor
```

退出串口监视器：`Ctrl+]`。

## 验证标志

正常启动时应看到以下关键信息：

```text
LT9611: SDA=7 SCL=8 RESET=23, I2C=0x3b, 720p60 RGB888, 2 lanes
LT9611 chip ID: 17 02 e1
HPD: connected
vtotal=750, vactive=720
hactive primary=1280
```

屏幕已经使用本工程对应源码和配置实际点亮。更详细的寄存器、日志结论和排查过程见：

- `examples/display/LT9611-validation.md`
- `examples/display/K230-LT9611-reference.md`

## 目录说明

- `examples/display/`：可直接编译和烧录的显示示例
- `bsp/esp32_p4_function_ev_board/`：Nano 所用 P4 BSP 及 LT9611 接入
- `components/lcd/esp_lcd_lt9611/`：LT9611 驱动
- `components/esp_lvgl_port/`：LVGL 显示适配
- `examples/display/managed_components/`：本次构建锁定的托管依赖快照

工程基于 Espressif `esp-bsp` 提交 `a72098007376dd7a07f0cecdc87d97f52e5f2cc1`。各上游组件保留原有许可证；本仓库新增和修改部分按 Apache License 2.0 发布。
