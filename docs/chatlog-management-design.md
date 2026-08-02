# ChatLog 目录管理 — 实施方案

> 目标：参考录音文件管理方式，为 chatlog 目录增加查看/摘要/播放能力，通过 MCP 工具语音调用 + 串口命令。
> 依据：录音管理（`waveshare-s3-rlcd-4.2.cc` 录音模块）+ chatlog 存储（`chat_log.cc`）探索报告。

---

## 1. 背景与现状

### 1.1 chatlog 存储现状（已实现，写入侧）
- 目录：`/sdcard/logs/chatlogs/`（`chat_log.cc:14`）
- 命名：`chat_<YYYYMMDD>_<HHMMSS>_<topic>.txt` + `.wav`（⚠️ 时间戳为**会话结束时间**，由 `RenameFilesWithTopic` 在关闭时重新生成）
- `.txt`：JSONL，每行 `{"ts":"YYYY-MM-DD HH:MM:SS","role":"user|assistant","text":"..."}`
- `.wav`：24kHz / 2ch / 16bit PCM，**与录音格式完全一致**；ch0=麦克风，ch1=AI 喇叭（AEC 回采）
- 会话边界：音频通道 open→close 为一次会话；topic 取首句用户 STT

### 1.2 录音管理模式（参考模板）
- 目录 `/sdcard/records/`，`InitializeSdCard()` 创建
- MCP 工具：`list_recordings`/`play_recording`/`record_audio`/`delete_recording`（InitializeTools 内 AddTool）
- 串口 `LIST` → `ListRecordings()`（printf 文本，`RECORDINGS_START/END` 包裹）
- 辅助：`ListRecordingsJson`（cJSON，newest-first top-10）、`PlayRecordingByName`、`PlayRecordingPath`（2ch→1ch downmix）、`DeleteRecordingByName`
- `ResumeAudioService()` 恢复 AI 音频

### 1.3 关键复用点
- ✅ chatlog WAV 与录音 WAV **格式完全一致** → `PlayRecordingPath` 的读取/跳头/进度逻辑可直接复用
- ✅ `InitializeSdCard()`、`ResumeAudioService()`、`ShowNotify()`、WAV 时长公式均可复用
- ✅ MCP 工具注册模式（AddTool + PropertyList）可直接套用

---

## 2. 设计方案

### 2.1 新增 MCP 工具（4 个，语音可调用）

| 工具 | 参数 | 返回 | 功能 |
|---|---|---|---|
| `self.list_chatlogs` | 无 | cJSON | 列出最近 10 条 chatlog（name/txt_size/wav_size/duration_seconds/started_at） |
| `self.get_chatlog_summary` | `filename`(string) | cJSON/string | 解析 `.txt` JSONL，返回对话摘要（最多 50 轮） |
| `self.play_chatlog_audio` | `filename`(string), `channel`(string, 默认"mixed") | string | 播放配对 `.wav`，声道可选 mixed/mic/ai |
| `self.delete_chatlog` | `filename`(string) | string | 删除 chatlog（.txt + 配对 .wav） |

**工具描述（写给 LLM，含中文触发词）**：
- `list_chatlogs`：用户问"最近聊了什么"、"对话记录"、"聊天历史" → 列出
- `get_chatlog_summary`：用户问"那次对话说了什么"、"对话内容" → 摘要
- `play_chatlog_audio`：用户问"听一下那段录音"、"播放对话" → 播放；可选声道：mixed(默认，双方)/mic(只听用户)/ai(只听小智)
- `delete_chatlog`：用户问"删掉那条对话"、"清除记录" → 删除

### 2.2 新增串口命令（1 个）

| 命令 | 功能 | 输出格式 |
|---|---|---|
| `CHATLOGLIST` | 列出 chatlog 目录 | `CHATLOGS_START ... CHATLOGS_END (N files)` |

> 注：`CHATLOG` 已被占用（触发合成会话），故用 `CHATLOGLIST`。

### 2.3 新增辅助函数（5 个，均在 CustomBoard 类）

| 函数 | 签名 | 说明 |
|---|---|---|
| `GetChatlogPath` | `void GetChatlogPath(char* buf, size_t len, const char* filename, const char* ext)` | 路径 helper：拼 `/sdcard/logs/chatlogs/<base>.<ext>`，避免重复 strncpy |
| `ListChatlogs` | `void ListChatlogs()` | 串口文本列表（镜像 `ListRecordings`） |
| `ListChatlogsJson` | `cJSON* ListChatlogsJson()` | MCP JSON（镜像 `ListRecordingsJson`，filter `chat_*.txt`） |
| `GetChatlogSummaryJson` | `cJSON* GetChatlogSummaryJson(const char* filename)` | 解析 JSONL 返回对话数组（最多 50 轮） |
| `PlayChatlogPath` | `void PlayChatlogPath(const char* path, int channel_mode)` | 播放 wav，channel_mode: 0=mixed/1=mic/2=ai |
| `PlayChatlogByName` | `bool PlayChatlogByName(const char* filename, int channel_mode)` | 播放配对 wav（spawn task） |
| `DeleteChatlogByName` | `bool DeleteChatlogByName(const char* filename)` | 删除 .txt + 配对 .wav |

### 2.4 关键设计决策

#### D1：播放声道策略（可切换）
- **录音**：`mono[i] = stereo[i*2]`（只取 ch0 麦克风）
- **chatlog**：声道可切换，由 `play_chatlog_audio` 的 `channel` 参数控制：
  - `"mixed"`(默认)：`mono[i] = (stereo[i*2] + stereo[i*2+1]) / 2`（平均混合双方对话）
  - `"mic"`：`mono[i] = stereo[i*2]`（只听用户麦克风 ch0）
  - `"ai"`：`mono[i] = stereo[i*2+1]`（只听小智 AI 喇叭 ch1）

> 实现：新增 `PlayChatlogPath(path, channel_mode)`，channel_mode 为枚举（0/1/2）。LLM/用户可通过自然语言指定（"只听小智的声音"→ai）。

#### D2：文件列表基准
- 以 `.txt` 为条目（每条 chatlog 必有 .txt），附带配对 `.wav` 的 size/duration
- 列表显示 `name`（含 .txt 扩展名），摘要/播放时由 helper 自动推导 base 名查配对文件

#### D3：摘要解析
- `fopen` .txt，`fgets` 逐行，`cJSON_Parse` 每行
- 累积成 `{"topic":"...","turns":[{"ts","role","text"}],"turn_count":N}` 结构
- 防御：最后一行可能未写完（断电），解析失败跳过；限制最多 50 轮避免超大响应

#### D4：并发保护
- 复用录音的 `static char play_path[]` 模式但用独立 buffer（避免与录音播放冲突）
- 可选：加 `volatile bool chatlog_playing_` 守卫（镜像 `music_playing_`）

#### D5：AI 音频暂停/恢复
- 播放前 `audio.Stop()` + `vTaskDelay(100ms)` + `codec->EnableOutput(true)/EnableInput(false)`
- 播放后 `ResumeAudioService()`（与录音/音乐一致）

---

## 3. 评审与优化

### 3.1 自评审发现的问题

| # | 问题 | 优化方案 |
|---|---|---|
| R1 | ⚠️ 时间戳是会话**结束**时间，用户可能困惑 | list 时同时返回 JSONL 首行 `ts`（真实开始时间）作为 `started_at` 字段 |
| R2 | `.txt` 的 text 字段未转义换行符，可能破坏 JSONL 单行 | 摘要解析用 `fgets` 整行读，cJSON 解析失败即跳过该行（容错） |
| R3 | 播放长对话 wav 可能数分钟，需可中途停止 | 复用录音播放的无中断设计（先不做停止）；或加 `self.stop_chatlog`（YAGNI，暂不做） |
| R4 | chatlog 与录音/音乐播放可能并发冲突 | 加 `chatlog_playing_` 守卫，与 `music_playing_` 互斥检查 |
| R5 | 文件名含中文 topic，printf/fgets 需 UTF-8 安全 | FATFS 已配 codepage 936 + UTF-8 API，dirent/fOPEN 均安全（已验证于音乐播放） |
| R6 | 配对 .wav 可能不存在（会话刚开始只有 .txt） | `PlayChatlogByName` 先 stat .wav，不存在返回错误提示 |

### 3.2 采纳的优化
- **采纳 R1**：list 返回 `started_at`（解析首行 ts）
- **采纳 R4**：加 `chatlog_playing_` 守卫，与 music 互斥
- **采纳 R6**：播放前校验 .wav 存在
- **暂不采纳 R3**：停止命令留作后续（YAGNI）

### 3.3 风险评估
- **低风险**：纯新增代码，不改动现有 chatlog 写入逻辑；复用成熟的录音播放路径
- **中风险**：双声道混合音质（麦克风+AI 喇叭叠加可能失真）—— 实测后可调整策略（如只播 ch1=AI，或可切换）
- **依赖**：SD 卡已挂载（InitializeSdCard 早返回）；chatlog 目录已由 chat_log.cc 创建

---

## 4. 实施计划

### 阶段 1：辅助函数（waveshare-s3-rlcd-4.2.cc）
1. 加 `GetChatlogPath` helper
2. 加 `ListChatlogs()`（串口文本，filter `chat_*.txt`）
3. 加 `ListChatlogsJson()`（cJSON，含 started_at）
4. 加 `GetChatlogSummaryJson(filename)`（JSONL 解析）
5. 加 `PlayChatlogPath(path)`（双声道混合播放）
6. 加 `PlayChatlogByName(filename)`（spawn task + .wav 校验）
7. 加 `volatile bool chatlog_playing_` 成员 + 互斥守卫

### 阶段 2：MCP 工具注册（InitializeTools 内）
8. 注册 `self.list_chatlogs`
9. 注册 `self.get_chatlog_summary`
10. 注册 `self.play_chatlog_audio`

### 阶段 3：串口命令（ScreenshotCmdTask）
11. 加 `CHATLOGLIST` 分支 → `ListChatlogs()`

### 阶段 4：文档与验证
12. 更新 `docs/usage.md` MCP 工具表 + 串口命令表
13. 更新板 `README.md` MCP 工具表
14. 编译验证（零警告）
15. 烧录 + 串口 `CHATLOG`(造数据) + `CHATLOGLIST` 实测
16. 语音实测（"最近聊了什么" / "听一下对话"）

### 文件改动清单
| 文件 | 改动 |
|---|---|
| `main/boards/waveshare-s3-rlcd-4.2/waveshare-s3-rlcd-4.2.cc` | +6 函数 +3 MCP 工具 +1 串口命令 +1 成员 |
| `docs/usage.md` | MCP 表 + 串口表 + 对话示例 |
| `main/boards/waveshare-s3-rlcd-4.2/README.md` | MCP 工具表 |

---

## 5. 预期 MCP 返回示例

### list_chatlogs
```json
{
  "count": 3,
  "chatlogs": [
    {"name":"chat_20260802_143000_天气查询.txt", "txt_size":412, "wav_size":1152044, "duration_seconds":6.0, "started_at":"2026-08-02 14:29:50"},
    ...
  ]
}
```

### get_chatlog_summary
```json
{
  "filename":"chat_20260802_143000_天气查询.txt",
  "topic":"天气查询",
  "turn_count":2,
  "turns":[
    {"ts":"2026-08-02 14:29:50","role":"user","text":"今天天气怎么样"},
    {"ts":"2026-08-02 14:29:52","role":"assistant","text":"今天晴朗，26度"}
  ]
}
```

### play_chatlog_audio
```
"Playing chatlog: chat_20260802_143000_天气查询.wav"
```
（播放期间底部通知显示进度百分比）

---

## 6. 待确认决策点

1. **播放声道**：平均混合 vs 只播 AI 声道(ch1) vs 可切换？默认采用平均混合
2. **是否需要 `self.delete_chatlog`**：录音有 delete，chatlog 是否需要？默认暂不加（YAGNI）
3. **摘要最大轮数**：默认 50 轮，是否够？
4. **是否现在实施**：方案确认后立即编码，还是先评审调整？
