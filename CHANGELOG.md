# 变更记录 (CHANGELOG)

本文件记录固件版本变更历史。每个版本按时间倒序排列，包含变更摘要与新功能/修复/变更分类。

> **规则**：每次版本 bump 必须更新本文件（见 AGENTS.md 版本管理章节）。

---

## v2.4.0 — 2026-08-02

### feat
- **list_chatlogs 支持系统日志目录**：`self.list_chatlogs` 新增可选 `directory` 参数（`chatlogs` 默认 / `system_logs` / `logs`），可语音查询 `/sdcard/logs/` 下的系统日志文件（`log_YYYYMMDD.txt`）
- 新增 `ListSystemLogs()` / `ListSystemLogsJson()` 辅助函数
- 新增 `SYSLOGLIST` 串口命令（列系统日志）

---

## v2.3.0 — 2026-08-02

### feat
- **ChatLog 目录管理 MCP 工具**（4 个语音可调用工具）：
  - `self.list_chatlogs`：列出最近 10 条对话（name/topic/大小/时长/真实开始时间）
  - `self.get_chatlog_summary`：解析 JSONL 返回对话摘要（最多 50 轮）
  - `self.play_chatlog_audio`：播放对话音频，声道可切换（mixed/mic/ai）
  - `self.delete_chatlog`：删除对话（.txt + 配对 .wav）
- 新增 `CHATLOGLIST` 串口命令
- 新增 8 个辅助函数：`BuildChatlogPath`、`TopicFromChatlogName`、`ListChatlogs(+Json)`、`GetChatlogSummaryJson`、`PlayChatlogPath`（声道可切换 + 可中断）、`PlayChatlogByName`、`DeleteChatlogByName`
- 播放守卫 `chatlog_playing_` 与 `music_playing_` 互斥

---

## v2.2.0 — 2026-08-02

### feat
- **固件版本信息 MCP 工具**：`self.get_version_info`（语音可调用），返回版本号、构建时间、git 提交、IDF 版本、ELF SHA、分区表、功能清单、烧录指引
- 新增 `main/version_info.{h,cc}`：`VersionInfo::GetVersionString()` / `GetFeatureListJson()` / `BuildVersionInfoJson()`
- 新增 `GIT_COMMIT` 宏捕获（`main/CMakeLists.txt` 中 `git rev-parse --short HEAD`，失败回退 `unknown`）
- 更新 `docs/usage.md` 与板 README MCP 工具表

### fix
- **C1 通知空闲时钟抢占修复**：`ShowNotification()` 重置 `last_status_update_time_` 使空闲时钟窗口重开；`UpdateStatusBar()` 空闲时钟分支在通知可见时跳过 `SetStatus(时钟)`——短通知（3s）与长通知（30s）均可完整显示满 duration_ms
- 新增 `docs/notification-display-analysis.md` 分析报告

---

## v2.1.0 — 基线版本

### feat（本分支定制功能线）
- AAC/M4A 音乐支持、中文主题文件名、线程安全时间
- 对话日志落盘（ChatLog）
- MP3 音乐播放、硬件自检、中文文件名支持、音频服务修复
- SD 卡录音/回放、SD 日志 tee（fsync 节流）、MCP 音频工具、KEY 三击诊断
- KEY 三功能按键、图标轮播显示、通知布局调整
- PCF85063 RTC 集成（开机即时时钟，NTP 回写）
- 温湿度/电量 MCP 查询工具
- 截图能力（串口 SHOOT + BOOT 长按 + 开机自动截图）
