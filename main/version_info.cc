#include "version_info.h"

#include "board.h"
#include "system_info.h"

#include <esp_app_desc.h>

#include <cstdio>
#include <string>
#include <vector>

namespace VersionInfo {

static const char* kFeatures[] = {
    "语音交互：离线唤醒词、流式语音识别与语音合成，支持中文/英文/日文",
    "通信：WebSocket 与 MQTT+UDP 双协议，支持 WiFi 连接",
    "OPUS 音频编解码与流式 ASR+LLM+TTS 对话",
    "设备端 MCP 工具：音箱、屏幕、温湿度、电量、录音、音乐、日志等语音可控",
    "4.2 寸圆形单色 LCD 显示，支持表情动画与状态轮播",
    "音乐播放：支持 SD 卡 MP3/AAC/M4A 音乐，自动解码重采样，中文文件名",
    "SD 卡录音与回放，支持一键录音与语音回放",
    "对话日志 ChatLog：AI 对话文本+语音自动保存到 SD 卡",
    "ChatLog 管理：语音查询对话历史/摘要/播放/删除，支持声道切换",
    "日志查询：语音列出对话日志与系统日志（ESP_LOG tee）",
    "BOOT 三击全屏版本信息：版本号+变更摘要+按键说明（14px字体，15秒）",
    "传感器历史：每5分钟自动采样温湿度/电量到SD卡，HTTP页面查看趋势",
    "定时器/闹钟：语音设定倒计时提醒，到时播放提示音",
    "WiFi HTTP 文件服务器：mDNS(xiaozhi.local)、在线播放 WAV、ChatLog 查看、文件删除、WiFi 热更新、OTA 在线升级、10分钟自动关闭",
    "固件版本信息：语音查询版本号、构建信息、git 提交、功能清单、烧录信息",
    "温湿度传感器（SHTC3）与实时时钟（PCF85063 RTC），支持 NTP 校时",
    "电池监测与低电量提醒，支持充电状态显示",
    "硬件自检：7 项（显示/按键/SD 卡/电池/RTC/温湿度/音频）",
    "本地截图：串口或按键触发，PBM 输出",
    "蓝牙键盘输入：BLE HID Host 连接键盘，快捷键控制（对话/音量/截图/录音等），串口 BTSCAN 配对",
};

std::string GetVersionString() {
    auto app_desc = esp_app_get_description();
    char buf[256];
    snprintf(buf, sizeof(buf), "%s v%s (build %sT%sZ, git %s, ESP-IDF %s)",
             app_desc->project_name, app_desc->version,
             app_desc->date, app_desc->time, GIT_COMMIT, app_desc->idf_ver);
    return std::string(buf);
}

std::string GetFeatureListJson() {
    std::string json = "[";
    for (size_t i = 0; i < sizeof(kFeatures) / sizeof(kFeatures[0]); i++) {
        if (i > 0) {
            json += ",";
        }
        json += R"(")" + std::string(kFeatures[i]) + R"(")";
    }
    json += "]";
    return json;
}

std::string BuildVersionInfoJson() {
    // Reuse the rich system-info payload built by the board: version, compile
    // time, IDF version, ELF SHA256, partition table, booted OTA slot, etc.
    auto& board = Board::GetInstance();
    std::string json = board.GetSystemInfoJson();

    // Remove the trailing "}" so we can inject extra fields, then re-close.
    json.pop_back();

    json += R"(,"firmware_description":")" + GetVersionString() + R"(")";
    json += R"(,"git_commit":")" + std::string(GIT_COMMIT) + R"(")";
    json += R"(,"features":)" + GetFeatureListJson();
    json += R"(,"flash_note":")";
    json += "烧录请使用 idf.py -p COMx flash，分区为双 OTA 槽（ota_0/ota_1）+ assets 分区，";
    json += "详见 docs/usage.md 与 README.md 烧录章节";
    json += R"(")";

    json += "}";
    return json;
}

}  // namespace VersionInfo
