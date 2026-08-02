#ifndef VERSION_INFO_H
#define VERSION_INFO_H

#include <string>

// Build-time git commit short hash (defined via main/CMakeLists.txt).
#ifndef GIT_COMMIT
#define GIT_COMMIT "unknown"
#endif

// Firmware display name (project name from CMake, "xiaozhi").
#ifndef PROJECT_NAME
#define PROJECT_NAME "xiaozhi"
#endif

namespace VersionInfo {

// Human-friendly description of this firmware build, e.g.
// "xiaozhi v2.2.0 (build 2026-08-02T14:30:00Z, git abc1234, ESP-IDF v5.5.x)".
std::string GetVersionString();

// The list of features supported by this firmware, used by the voice
// queryable self.get_version_info MCP tool. Each entry is a short
// human-readable phrase (Chinese) that the LLM can paraphrase in speech.
// Keep this in sync with the board README "新增功能" section.
std::string GetFeatureListJson();

// Build a complete version-info JSON payload for the voice queryable
// MCP tool. Reuses Board::GetSystemInfoJson() and appends:
//   "git_commit", "features" (array), "firmware_description",
//   "flash_note" (static flash guidance text).
std::string BuildVersionInfoJson();

}  // namespace VersionInfo

#endif  // VERSION_INFO_H
