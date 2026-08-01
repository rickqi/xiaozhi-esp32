#ifndef CHAT_LOG_H
#define CHAT_LOG_H

#include <string>
#include <vector>
#include <mutex>
#include <functional>

/**
 * ChatLog - records AI chat conversations to SD card.
 *
 * Directory layout:
 *   /sdcard/logs/chatlogs/
 *     chat_<YYYYMMDD>_<HHMMSS>_<topic>.txt   (JSONL: one chat turn per line)
 *     chat_<YYYYMMSS>_<HHMMSS>_<topic>.wav   (24kHz stereo: ch0=mic, ch1=AEC ref/speaker)
 *
 * Lifecycle:
 *   BeginConversation(topic)  - create the per-conversation .txt/.wav files
 *   LogUser(text)             - append a user turn (role "user")
 *   LogAssistant(text)        - append an assistant turn (role "assistant")
 *   WriteInputPcm(pcm)        - append interleaved (mic, ref) PCM to the .wav
 *   WriteOutputPcm(pcm)       - optional: append mono speaker PCM (mixed into ref)
 *   EndConversation()         - finalize WAV header, fsync, close
 */
class ChatLog {
public:
    ChatLog() = default;
    ~ChatLog();

    // SD card readiness gate - returns true if /sdcard is mounted.
    // The caller provides this; default uses stat("/sdcard").
    using SdReadyFn = std::function<bool()>;
    void SetSdReadyFn(SdReadyFn fn);

    bool InConversation() const { return active_; }

    // Start a conversation with a short topic (may be empty).
    // Returns true on success (SD mounted + files opened).
    bool BeginConversation(const std::string& topic);

    // Set/update the conversation topic. Applied to filenames on close.
    void SetTopic(const std::string& topic);

    // Append a chat turn to the JSONL file.
    void LogUser(const std::string& text);
    void LogAssistant(const std::string& text);

    // Append audio to the conversation .wav.
    // pcm must be interleaved stereo (mic, AEC-reference) at 24kHz.
    void WriteInputPcm(const std::vector<int16_t>& pcm);
    // Mono speaker PCM at 24kHz; mixed into the reference channel.
    void WriteOutputPcm(const std::vector<int16_t>& pcm);

    // Finalize and close the current conversation files.
    void EndConversation();

private:
    bool OpenFiles(const std::string& topic);
    void WriteJsonLine(const std::string& role, const std::string& text);
    void AppendWav(const int16_t* data, size_t samples, bool stereo);
    void ThrottledFsync();

    bool active_ = false;
    std::string txt_path_;
    std::string wav_path_;
    std::string topic_;              // topic set during conversation
    FILE* txt_file_ = nullptr;
    FILE* wav_file_ = nullptr;
    uint32_t wav_data_len_ = 0;
    std::mutex mutex_;
    int64_t last_fsync_ms_ = 0;
    SdReadyFn sd_ready_;

    static std::string MakeTimestamp();
    static std::string SanitizeTopic(const std::string& topic, size_t max_len);
    void WriteWavHeader(FILE* f);
    void FinalizeWavHeader(FILE* f);
    void RenameFilesWithTopic();
};

#endif // CHAT_LOG_H
