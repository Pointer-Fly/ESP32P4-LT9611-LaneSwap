# ESP32-P4 Nano + LT9611 HDMI 验证结果

更新时间：2026-09-22。

## 结论

验证通过。更换为引脚兼容的普通 LT9611 后，I2C、芯片初始化、MIPI DSI 接收、HDMI HPD 和实际显示均正常，显示器已成功亮屏。

最终固件运行参数：

- ESP32-P4 Nano revision v1.3，CPU 360 MHz。
- LT9611 控制 I2C：SDA GPIO7、SCL GPIO8、7 位地址 `0x3b`、400 kHz。
- RESET：GPIO23，低电平有效。
- 1280×720@60 Hz、RGB888、2 条 DSI data lane、960 Mbit/s/lane。
- 物理接线：P4 D0 → LT9611 Port-B D2（P/N 交叉），P4 D1 → Port-B D3，CLK → Port-B CLK。

## 最终实测

```text
SW I2C ACK at 0x3b
Chip ID registers: 17 02 e1; input port B
MIPI input config: 2 lanes, Port B, route 0x62 (K230 reverse lane map)
LT9611 initialization done (HPD=yes)
RX counters: vtotal=750 vactive=720 htotal_sys=600
Display initialized
Display resolution 1280x720
Display LVGL animation
RX status: HPD=yes vtotal=750 vactive=720 htotal_sys=600/601
           hactive_a=1280 (3840 RGB888 bytes), hactive_b=0, format=0x0a
```

延时 1、2、3 秒读取均稳定。`hactive_a` 是芯片内部主视频检查路径的旧命名；物理 Port B 经 port swap 成为 primary，所以数据出现在逻辑 A 计数器。`3840 / 3 = 1280` 与 RGB888 宽度吻合。

## 验证通过的 Port-B 双 lane 配置

```text
0x8250 = 0x14
0x8300 = 0x62
0x8303 = 0x4f
0x8304 = 0x00
0x8307 = 0x40
0x824f = 0x80
0x8302 = 0x08
0x8306 = 0x08
0x830a = 0x00
```

`0x62` 由 K230 四 lane 板级路由高位 `0x60` 与普通 LT9611 双 lane 值 `0x02` 组合，并已在本板实测通过。公开资料只明确说明 `0x8303[6]` 用于 Port-B primary、`0x8250[3:2]` 用于 Port-B byte clock，以及双 lane 值为 `0x02`；其余板级位域没有公开定义，因此这组值应视为本板已验证配置。

组件增加了独立的 `k230_port_b_lane_map` vendor flag。BSP 只在本板 LT9611 配置中同时启用 `input_port_b` 和该 flag，不改变普通 Port-B 接线的默认行为。

## 编译与烧录

```sh
. "$IDF_PATH/export.sh"
cd examples/display

idf.py -B build_lt9611 \
  -D SDKCONFIG=sdkconfig.lt9611 \
  -D SDKCONFIG_DEFAULTS=sdkconfig.bsp.waveshare_p4_nano_lt9611 \
  build

idf.py -B build_lt9611 -p /dev/ttyACM0 flash monitor
```

最终编译成功，应用大小约 `0xBCE30`，1 MiB 应用分区剩余约 26%。Bootloader、分区表、应用三段写入均完成 hash 校验，源码通过 `git diff --check`。

## 主要改动

- BSP 新增 LT9611 HDMI 配置与 GPIO23 复位配置。
- 适配 P4 v1.3：选择旧版芯片 revision 并使用 CPU 360 MHz，避免 400 MHz 启动断言。
- LT9611 组件增加 Port B、K230 反向 lane 映射、芯片 ID 和 RX 状态诊断。
- LVGL9 使用 ESP-IDF 6.0.2 支持的 `on_refresh_done` 回调。
- 保留验证所需诊断：I2C 扫描、GPIO 开漏回读及三个延时 RX 状态样本。

## 资料来源

- [Linux 主线 LT9611 Port B 支持](https://github.com/torvalds/linux/commit/e8bd92c4a0d2ee0fe8ee2b33c94dcffb3a39749b)
- [K230 固定版本 LT9611 驱动](https://github.com/canmv-k230/mpp/blob/7ff28cd1eef41ae73c442b81bec1803f68eebae9/kernel/connector/src/panels/hdmi_lt9611.c#L170-L193)
- [普通 LT9611 lane-count 参考](https://github.com/sipeed/MaixCDK/blob/main/components/3rd_party/sophgo-middleware/sophgo-middleware/v2/sample/mipi_tx/lt9611/lt9611.c#L2305-L2315)
