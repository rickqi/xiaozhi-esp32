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
    "双协议通信：WebSocket 与 MQTT+UDP 双协议，支持 WiFi 与 4G",
    "OPUS 音频编解码与流式 ASR+LLM+TTS 对话",
    "设备端 MCP 工具：音箱、屏幕、LED、温湿度、电量、录音、音乐等语音可控",
    "4.2 寸圆形单色 LCD 显示，支持表情动画与状态轮播",
    "音乐播放：支持 SD 卡 MP3/AAC/M4A 音乐，自动解码重采样，中文文件名",
    "SD 卡录音与回放，支持一键录音与语音回放",
    "对话日志 ChatLog：AI 对话文本+语音自动保存到 SD 卡",
    "温湿度传感器（SHTC3）与实时时钟（PCF85063 RTC），支持 NTP 校时",
    "电池监测与低电量提醒，支持充电状态显示",
    "硬件自检：7 项（显示/按键/SD 卡/电池/RTC/温湿度/音频）",
    "本地截图：串口或按键触发，PBM 输出",
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
    json += R"(,"git_commit_full":")" + std::string(GIT_COMMIT) + R"(")";
    json += R"(,"features":)" + GetFeatureListJson();
    json += R"(,"flash_note":")";
    json += "烧录请使用 idf.py -p COMx flash，分区为双 OTA 槽（ota_0/ota_1）+ assets 分区，";
    json += "详见 docs/usage.md 与 README.md 烧录章节";
    json += R"(")";

    json += "}";
    return json;
}

}  // namespace VersionInfo
