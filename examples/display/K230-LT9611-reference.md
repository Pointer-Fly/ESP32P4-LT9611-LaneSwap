# K230 LT9611 输入配置参考

用户提供的 `kendryte/canmv_k230` 仓库通过构建清单引用 CanMV K230 MPP 仓库中的硬件驱动。本工程使用其中的 Port-B 板级路由作为依据，并结合普通 LT9611 的双 lane 配置，在 ESP32-P4 Nano 上完成了实测。

## 来源链

- CanMV 提交：`b3211026add55b02dad803f6ec7b19e3fb978acf`
- 构建清单：<https://github.com/canmv-k230/manifest/blob/master/default.xml>
- MPP 提交：`7ff28cd1eef41ae73c442b81bec1803f68eebae9`
- 驱动：<https://github.com/canmv-k230/mpp/blob/7ff28cd1eef41ae73c442b81bec1803f68eebae9/kernel/connector/src/panels/hdmi_lt9611.c>

K230 的 `lt9611_panel_init()` 选择 `LT9611_PORTB`，`lt9611_mipi_input_digital()` 写入：

| 寄存器 | K230 四 lane 值 |
| --- | --- |
| `0x8250` | `0x14` |
| `0x8300` | `0x60` |
| `0x8303` | `0x4f` |
| `0x8304` | `0x00` |
| `0x8307` | `0x40` |
| `0x824f` | `0x80` |
| `0x8302` | `0x08` |
| `0x8306` | `0x08` |
| `0x830a` | `0x00` |

## ESP32-P4 双 lane 实测结果

本板接线为 DSI D0 → Port-B D2 且 P/N 反转、DSI D1 → Port-B D3、CLK 极性不反转。最终将 K230 的 `0x8300=0x60` 与普通 LT9611 双 lane 值 `0x02` 组合为 `0x62`，其余路由值保持上述组合。

该配置已在普通 LT9611 上获得稳定的 `1280×720@60 Hz RGB888` 输出：芯片 ID 为 `17 02 e1`，HPD 已连接，RX 计数为 `vtotal=750`、`vactive=720`、primary `hactive=1280`。

公开资料能够直接确认：

- `0x8303[6]=1` 使 Port B 成为内部 primary。
- `0x8250[3:2]=01` 选择 Port B byte clock。
- `0x8300` 的双 lane 基础值为 `0x02`。

其余位域没有公开的完整定义，因此不要把单个寄存器描述为完整的 lane swap 开关。本工程记录的是这一块 PCB 接线上的完整实测组合。

## 交叉参考

- K230 旧 SDK 使用相同输入序列：<https://github.com/kendryte/k230_sdk/blob/main/src/big/mpp/kernel/connector/src/lt9611.c>
- K230 Linux SDK 中保留相同序列：<https://github.com/kendryte/k230_linux_sdk/blob/e661aa44e4c76f98b04226bb8c2ef3fa835b42e4/buildroot-overlay/linux/0001-modify-lt9611-for-k230.patch>
- Linux 主线 Port-B 支持说明：<https://github.com/torvalds/linux/commit/e8bd92c4a0d2ee0fe8ee2b33c94dcffb3a39749b>
