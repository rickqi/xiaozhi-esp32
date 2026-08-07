# 硬件烧录测试指南

> 目标硬件：Waveshare ESP32-S3-Touch-AMOLED-2.06
> 分支：feature/brookesia-phone
> 固件：xiaozhi.bin (4.2MB)

## 1. 烧录

```bash
cd /home/xiaozhi-esp32
source /root/esp/esp-idf/export.sh

# 确认板卡选项
grep "BROOKESIA" sdkconfig
# 应显示: CONFIG_BOARD_TYPE_WAVESHARE_S3_TOUCH_AMOLED_2_06_BROOKESIA=y

# 烧录（替换 PORT）
idf.py -p /dev/ttyUSB0 flash monitor
# 或
idf.py -p /dev/ttyACM0 flash monitor
```

## 2. 验证清单

### 2.1 显示 + 触摸
- [ ] 屏幕亮起，显示 Phone Shell（状态栏 + App Launcher）
- [ ] 触摸滑动正常（App Launcher 翻页）
- [ ] 点击 XiaoZhi 图标进入聊天界面
- [ ] 聊天界面显示：情绪图标 + 聊天区域 + 状态栏

### 2.2 WiFi + 语音
- [ ] 首次启动进入配网模式（或串口发送配网指令）
- [ ] 连接 WiFi 后，说"你好小智"唤醒
- [ ] AI 回复在聊天气泡中显示
- [ ] 情绪图标随 AI 情绪变化（PNG 表情）

### 2.3 截屏
```bash
# 开发机运行截图服务器
python tools/screenshot_server.py

# 对 AI 说："请截屏并发送到 http://<开发机IP>:8899"
# 或在 xiaozhi web 控制台调用 self.screen.snapshot
```
- [ ] output/ 目录出现 screenshot_*.jpg

### 2.4 BLE 键盘
- [ ] 对 AI 说"扫描蓝牙键盘"
- [ ] 将 BLE 键盘进入配对模式
- [ ] 再次说"扫描蓝牙键盘"连接
- [ ] 说"键盘连上了吗"确认状态
- [ ] 按回车键切换对话

### 2.5 SD 卡
- [ ] 插入 microSD 卡
- [ ] 串口显示 "SD card mounted at /sdcard"
- [ ] 对 AI 说"列出录音"/"列出歌单"/"列出对话日志"
- [ ] 说"打开文件服务器"，浏览器访问设备 IP

### 2.6 定时器
- [ ] 说"设置一个一分钟的定时器"
- [ ] 一分钟后听到提示音 + 屏幕通知

## 3. 已知限制

| 限制 | 原因 | 解决方案 |
|------|------|---------|
| Flash 剩余 10% | brookesia fonts + Twemoji64 | 禁用不用的 brookesia 字体大小 |
| 无温度/湿度查询 | AMOLED 2.06 无 SHTC3 | 如需可外接传感器 |
| 无音乐播放 | 需迁移 esp_audio_codec 解码代码 | Phase 5 后续 |
| 无录音功能 | 需迁移 AudioCodec 录音代码 | Phase 5 后续 |
| 无硬件自检 | 需针对 AMOLED 2.06 重写 | 可选 |

## 4. 串口命令

```
BTSTATUS    — 查询 BLE 键盘状态
BTSCAN      — 扫描 BLE 键盘
HTTPSTART   — 启动文件服务器
HTTPSTOP    — 停止文件服务器
```
