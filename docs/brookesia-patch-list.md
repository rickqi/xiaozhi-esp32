# Brookesia 分支补丁清单

> 本文件记录 `feature/brookesia-phone` 分支对 xiaozhi stock 代码的所有修改。
> 合并 xiaozhi master 更新时参考此文件。

## 修改的文件（从 xiaozhi stock）

| 文件 | 修改类型 | 修改内容 | 合并策略 |
|------|---------|---------|---------|
| `main/idf_component.yml` | 替换 | `lvgl ~9.3.0` → `9.5.0`; `esp_lvgl_port ~2.6.0` → `~2.8.0` | `git checkout --ours` |
| `main/CMakeLists.txt` | 追加 | brookesia 源文件 + 组件链接（标记块） | 保留双方 |
| `main/Kconfig.projbuild` | 追加 | `CONFIG_BOARD_TYPE_*_BROOKESIA` 板选项 | 保留双方 |
| `sdkconfig.defaults` | 追加 | brookesia Kconfig 配置（标记块） | 保留双方 |

## 新建的文件/目录（xiaozhi stock 不存在 → 零冲突）

| 路径 | 用途 |
|------|------|
| `components/brookesia_core/` | Phone Shell UI 框架（从例程复制） |
| `partitions-brookesia.csv` | 16MB OTA 分区表 |
| `scripts/check_interface_changes.sh` | 接口变更检测脚本 |
| `scripts/merge_xiaozhi_update.sh` | 半自动合并脚本 |
| `docs/brookesia-patch-list.md` | 本文件 |

## 后续 Phase 2-4 将新建

| 路径 | 用途 |
|------|------|
| `main/display/brookesia_display/` | Display 适配器 |
| `main/xiaozhi_app/` | Phone Shell App |
| `main/boards/.../esp32-s3-touch-amoled-2.06-brookesia.cc` | brookesia 版板卡 |
