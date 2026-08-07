#!/bin/bash
# 检测 xiaozhi 关键接口文件在合并后是否有变更
# 用法: ./scripts/check_interface_changes.sh [merge-base] [merged-head]
#   默认对比 HEAD~1..HEAD

set -e

MERGE_BASE="${1:-HEAD~1}"
MERGED="${2:-HEAD}"

INTERFACE_FILES=(
    "main/display/display.h"
    "main/boards/common/board.h"
    "main/mcp_server.h"
    "main/application.h"
    "main/boards/common/bluetooth_keyboard.h"
)

echo "=== 接口变更检测 ==="
echo "对比: $MERGE_BASE..$MERGED"
echo ""

CHANGED=0
for file in "${INTERFACE_FILES[@]}"; do
    if git cat-file -e "$MERGED:$file" 2>/dev/null && \
       ! git diff --quiet "$MERGE_BASE" "$MERGED" -- "$file" 2>/dev/null; then
        echo "⚠️  变更: $file"
        NEW_VIRTUAL=$(git diff "$MERGE_BASE" "$MERGED" -- "$file" \
            | grep '^+.*virtual' | grep -v '^+++' || true)
        if [ -n "$NEW_VIRTUAL" ]; then
            echo "   新增虚函数:"
            echo "$NEW_VIRTUAL" | sed 's/^/     /'
        fi
        CHANGED=1
    fi
done

if [ $CHANGED -eq 0 ]; then
    echo "✅ 关键接口文件无变更"
else
    echo ""
    echo "⚠️  请检查以下适配器是否需要更新："
    echo "   - main/display/brookesia_display/brookesia_display.h"
    echo "   - main/xiaozhi_app/xiaozhi_app.h"
    echo "   - 对应的板卡文件"
fi
