| Supported Targets | ESP32-P4 |
| ----------------- | -------- |

# ESP32-P4 Nano + LT9611 1080p60 MJPEG 播放器

本工程基于 Espressif `hdmi_video_renderer` 示例，适配微雪 ESP32-P4 Nano v1.3 与普通版 LT9611 转接板，从板载 TF 卡循环播放 MJPEG/MP4 视频。

当前配置：

- HDMI 输出：1920×1080@60 Hz，CEA VIC 16
- LCD 输入格式：RGB565
- MIPI DSI：2 data lanes，1280 Mbit/s/lane
- LCD 内部帧缓冲：3 个 1920×1080 RGB565 framebuffer
- 视频：1920×1072、YUV420、MJPEG、MP4，推荐先用 15 fps
- ESP32-P4：revision v1.3，CPU 360 MHz，PSRAM 200 MHz
- LT9611：I2C `0x3b`，RESET GPIO23

## 为什么视频是 1920×1072

ESP32-P4 硬件 JPEG 解码 YUV420 时，宽和高都必须是 16 的整数倍。1920 能被 16 整除，1080 不能，而 1072 可以。

原始示例同时要求 LCD 内部缓冲模式下视频尺寸必须与 HDMI 输出一致，这会让 1080p 与 YUV420 的 16 行对齐发生冲突。本工程对帧缓冲路径做了定向修改：

1. HDMI 和 LCD framebuffer 保持 1920×1080@60。
2. JPEG 硬件把 1920×1072 图像直接解码到空闲 framebuffer 的第 4 行。
3. framebuffer 顶部和底部各保留 4 行黑色。
4. 切换完整的 1920×1080 framebuffer，不做逐帧整屏复制或缩放。

这样既满足 YUV420 MCU 对齐，也保留显示器要求的标准 1080p60 时序。上下 4 像素黑边通常不可见。

## 硬件连接

| ESP32-P4 Nano | LT9611 | 说明 |
| --- | --- | --- |
| GPIO7 | SDA | I2C 数据 |
| GPIO8 | SCL | I2C 时钟 |
| GPIO23 | RESET | 低电平复位 |
| DSI CLK P/N | Port-B CLK P/N | 极性不反转 |
| DSI D0 P/N | Port-B D0 P/N | 直通，极性不反转 |
| DSI D1 P/N | Port-B D1 P/N | 直通，极性不反转 |
| GND | GND | 共地 |

驱动使用 Port-B 双 lane 直通配置，不启用 K230 的反向 lane map：

```text
8250=14  8300=02  8303=40  824f=80
8302=0a  8306=0a  830a=00
```

TF 卡使用 Nano 板载卡槽，程序会把 GPIO45 拉低来开启供电。

## 准备视频

播放器目前只支持 MP4 容器中的 MJPEG 视频轨，不支持 H.264 或 H.265。将转换后的文件复制到 FAT32 TF 卡根目录，并命名为：

```text
/test_video_1080p.mp4
```

仓库提供转换脚本 `tools/convert_mjpeg_1920x1072.sh`。它使用 ImageMagick 生成标准 YUV420 JPEG 采样布局，再把 JPEG 帧无损封装进 MP4：

```bash
sudo apt install ffmpeg imagemagick
./tools/convert_mjpeg_1920x1072.sh input.mp4 test_video_1080p.mp4 15
```

不要直接依赖 `ffmpeg -c:v mjpeg -pix_fmt yuvj420p`。部分 ffmpeg 版本会产生 ESP32-P4 JPEG 驱动不能正确识别的 SOF 采样布局。正确首帧应是：

```text
1920x1072
YUV420 sampling: Y=0x22, Cb=0x11, Cr=0x11
```

转换脚本默认去掉音频，以便先验证显示带宽。如果需要音频，可把原视频音轨转成 AAC 后与已生成的 MJPEG 视频轨重新封装；LT9611 驱动本身不会把音频嵌入 HDMI，声音从 Nano 的板载音频输出。

## 编译

使用 ESP-IDF 6.0.2：

```bash
source /root/.espressif/v6.0.2/esp-idf/export.sh

cd examples/hdmi_video_renderer
idf.py \
  -B build_lt9611_1080p \
  -D SDKCONFIG=sdkconfig.lt9611_1080p \
  -D SDKCONFIG_DEFAULTS=sdkconfig.defaults \
  build
```

## 烧录和串口监视

```bash
idf.py \
  -B build_lt9611_1080p \
  -D SDKCONFIG=sdkconfig.lt9611_1080p \
  -D SDKCONFIG_DEFAULTS=sdkconfig.defaults \
  -p /dev/ttyACM0 \
  flash monitor
```

按 `Ctrl+]` 退出串口监视器。

## 预期日志

```text
LT9611 uses direct Port-B map: D0->B0, D1->B1 (normal polarity)
LT9611: SDA=7 SCL=8 RESET=23, I2C=0x3b, 1080p60 RGB565, 2 lanes
Chip ID registers: 17 02 e1; input port B
LT9611 initialization done (HPD=yes)
RX counters: vtotal=1125 vactive=1080 ...
Video info: 1920x1072, 15 fps, ...
First JPEG hardware decode completed ... centered direct framebuffer path, sampling=YUV420 ... top_border=4
```

显示器应识别为 1920×1080@60 Hz。

## 带宽限制

1920×1080@60 RGB565 扫屏本身会持续读取约 249 MB/s 的 framebuffer 数据。JPEG 解码器同时向下一块 framebuffer 写入图像，因此这是接近 PSRAM 带宽上限的压力测试。工程已经采用 RGB565、三 framebuffer、直接解码、关闭 PSRAM XIP，并提高 DSI DMA 的 AXI 读优先级。

如果仍看到 `lcd.dsi ... underrun` 或显示器闪蓝，先把视频帧率从 15 fps 降到 10 fps；HDMI 输出仍保持 1080p60。提高 JPEG 质量会增大 SD 和压缩输入带宽，但最终 RGB565 解码帧的大小不变。

本工程基于 Espressif `esp-iot-solution` 的 `hdmi_video_renderer` 示例，并使用本地 BSP/LT9611 组件固定 Nano 接线和 lane swap 配置。
