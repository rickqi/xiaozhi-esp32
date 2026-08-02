#include "chat_log.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <esp_log.h>
#include <esp_timer.h>

#define TAG "ChatLog"

static const char* kChatLogsDir = "/sdcard/logs/chatlogs";
static const int kSampleRate = 24000;
static const int kChannels = 2;
static const int kBitsPerSample = 16;

ChatLog::~ChatLog() {
    EndConversation();
}

void ChatLog::SetSdReadyFn(SdReadyFn fn) {
    sd_ready_ = std::move(fn);
}

std::string ChatLog::MakeTimestamp() {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

std::string ChatLog::SanitizeTopic(const std::string& topic, size_t max_len) {
    std::string out;
    out.reserve(max_len + 2);
    for (char c : topic) {
        // Allow alphanumerics, CJK (multi-byte UTF-8), and a few safe
        // punctuation chars. Cast to unsigned char so high-bit bytes (>=0x80)
        // are correctly detected even though char is signed on Xtensa.
        unsigned char uc = static_cast<unsigned char>(c);
        bool printable = (uc >= 'a' && uc <= 'z') || (uc >= 'A' && uc <= 'Z') ||
                         (uc >= '0' && uc <= '9') || (uc >= 0x80) ||
                         (uc == '-' || uc == '_' || uc == '.');
        if (printable) {
            out += c;
        } else {
            out += '_';
        }
        if (out.size() >= max_len) break;
    }
    if (out.empty()) out = "chat";
    return out;
}

bool ChatLog::BeginConversation(const std::string& topic) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_) return false;

    bool sd_ok = false;
    if (sd_ready_) {
        sd_ok = sd_ready_();
    } else {
        struct stat st;
        sd_ok = (stat("/sdcard", &st) == 0);
    }
    if (!sd_ok) {
        ESP_LOGW(TAG, "SD not ready, conversation not logged");
        return false;
    }

    // Ensure the chatlogs directory exists (parent /sdcard/logs too).
    mkdir("/sdcard/logs", 0755);
    mkdir(kChatLogsDir, 0755);

    std::string stamp = MakeTimestamp();
    std::string topic_safe = SanitizeTopic(topic, 20);
    topic_ = topic_safe;

    // Open with timestamp-only base name; the topic is applied via rename at
    // EndConversation() so no data is lost when the topic becomes known later.
    char base[160];
    snprintf(base, sizeof(base), "%s/%s_%s", kChatLogsDir, "chat", stamp.c_str());
    txt_path_ = std::string(base) + ".txt";
    wav_path_ = std::string(base) + ".wav";

    txt_file_ = fopen(txt_path_.c_str(), "a");
    if (!txt_file_) {
        ESP_LOGE(TAG, "Cannot open chat text file %s errno=%d", txt_path_.c_str(), errno);
        return false;
    }
    wav_file_ = fopen(wav_path_.c_str(), "wb");
    if (!wav_file_) {
        ESP_LOGE(TAG, "Cannot open chat wav file %s errno=%d", wav_path_.c_str(), errno);
        fclose(txt_file_);
        txt_file_ = nullptr;
        return false;
    }
    WriteWavHeader(wav_file_);
    wav_data_len_ = 0;
    active_ = true;
    ESP_LOGI(TAG, "Conversation log started: %s / %s", txt_path_.c_str(), wav_path_.c_str());
    return true;
}

void ChatLog::SetTopic(const std::string& topic) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) return;
    topic_ = SanitizeTopic(topic, 20);
}

void ChatLog::WriteJsonLine(const std::string& role, const std::string& text) {
    if (!active_ || !txt_file_) return;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    // JSON-escape backslash and quotes.
    std::string escaped;
    escaped.reserve(text.size() + 8);
    for (char c : text) {
        if (c == '\\' || c == '"') escaped += '\\';
        escaped += c;
    }
    char line[40];
    strftime(line, sizeof(line), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(txt_file_, "{\"ts\":\"%s\",\"role\":\"%s\",\"text\":\"%s\"}\n",
            line, role.c_str(), escaped.c_str());
    fflush(txt_file_);
    ThrottledFsync();
}

void ChatLog::LogUser(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) return;
    ESP_LOGI(TAG, "chat user: %s", text.c_str());
    WriteJsonLine("user", text);
}

void ChatLog::LogAssistant(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) return;
    ESP_LOGI(TAG, "chat assistant: %s", text.c_str());
    WriteJsonLine("assistant", text);
}

void ChatLog::AppendWav(const int16_t* data, size_t samples) {
    if (!active_ || !wav_file_) return;
    size_t bytes = samples * sizeof(int16_t);
    fwrite(data, 1, bytes, wav_file_);
    wav_data_len_ += (uint32_t)bytes;
}

void ChatLog::WriteInputPcm(const std::vector<int16_t>& pcm) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) return;
    // pcm is interleaved stereo (mic, ref) at 24kHz.
    AppendWav(pcm.data(), pcm.size());
}

void ChatLog::WriteOutputPcm(const std::vector<int16_t>& pcm) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) return;
    // Mix mono speaker PCM into the reference channel so the .wav captures
    // both the user (mic ch0) and the AI voice (ref ch1). For each mono sample
    // we synthesize a stereo frame (mic=silence, ref=speaker sample) and append.
    // Simpler and robust: append as a left-silent stereo pair.
    static const int16_t kZero = 0;
    std::vector<int16_t> stereo;
    stereo.reserve(pcm.size() * 2);
    for (int16_t s : pcm) {
        stereo.push_back(kZero);
        stereo.push_back(s);
    }
    AppendWav(stereo.data(), stereo.size());
}

void ChatLog::ThrottledFsync() {
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - last_fsync_ms_ >= 1000) {
        if (txt_file_) fsync(fileno(txt_file_));
        if (wav_file_) fsync(fileno(wav_file_));
        last_fsync_ms_ = now_ms;
    }
}

void ChatLog::WriteWavHeader(FILE* f) {
    uint8_t hdr[44] = {0};
    memcpy(hdr, "RIFF", 4);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    hdr[16] = 16;  // fmt chunk size
    hdr[20] = 1;   // PCM
    hdr[22] = kChannels & 0xFF; hdr[23] = (kChannels >> 8) & 0xFF;
    hdr[24] = kSampleRate & 0xFF; hdr[25] = (kSampleRate >> 8) & 0xFF;
    hdr[26] = (kSampleRate >> 16) & 0xFF; hdr[27] = (kSampleRate >> 24) & 0xFF;
    uint32_t byte_rate = kSampleRate * kChannels * kBitsPerSample / 8;
    hdr[28] = byte_rate & 0xFF; hdr[29] = (byte_rate >> 8) & 0xFF;
    hdr[30] = (byte_rate >> 16) & 0xFF; hdr[31] = (byte_rate >> 24) & 0xFF;
    hdr[32] = kChannels * kBitsPerSample / 8;  // block align
    hdr[34] = kBitsPerSample;                  // bits per sample
    memcpy(hdr + 36, "data", 4);
    fwrite(hdr, 1, 44, f);
}

void ChatLog::FinalizeWavHeader(FILE* f) {
    if (!f) return;
    fflush(f);
    fseek(f, 4, SEEK_SET);
    uint32_t riff_size = 36 + wav_data_len_;
    fwrite(&riff_size, 1, 4, f);
    fseek(f, 40, SEEK_SET);
    fwrite(&wav_data_len_, 1, 4, f);
    fseek(f, 0, SEEK_END);
    fflush(f);
    fsync(fileno(f));
}

void ChatLog::RenameFilesWithTopic() {
    if (topic_.empty() || topic_ == "chat") return;
    // Build the final names with topic, then rename both files.
    char base[160];
    snprintf(base, sizeof(base), "%s/%s_%s_%s", kChatLogsDir, "chat", MakeTimestamp().c_str(), topic_.c_str());
    std::string new_txt = std::string(base) + ".txt";
    std::string new_wav = std::string(base) + ".wav";
    if (rename(txt_path_.c_str(), new_txt.c_str()) == 0) {
        txt_path_ = new_txt;
    }
    if (rename(wav_path_.c_str(), new_wav.c_str()) == 0) {
        wav_path_ = new_wav;
    }
}

void ChatLog::EndConversation() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_) return;
    if (wav_file_) {
        FinalizeWavHeader(wav_file_);
        fclose(wav_file_);
        wav_file_ = nullptr;
    }
    if (txt_file_) {
        fflush(txt_file_);
        fsync(fileno(txt_file_));
        fclose(txt_file_);
        txt_file_ = nullptr;
    }
    // Apply topic to filenames now that all handles are closed.
    RenameFilesWithTopic();
    active_ = false;
    ESP_LOGI(TAG, "Conversation log ended: %s (%u bytes audio)",
             txt_path_.c_str(), (unsigned int)wav_data_len_);
}
