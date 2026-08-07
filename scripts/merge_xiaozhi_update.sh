#!/bin/bash
# 半自动合并 xiaozhi master 更新到 brookesia 分支
# 用法: ./scripts/merge_xiaozhi_update.sh
#
# 流程:
#   1. fetch + merge
#   2. 自动解决已知冲突文件
#   3. 运行接口变更检测
#   4. 提示手动检查项

set -e

BRANCH=$(git branch --show-current)
if [ "$BRANCH" != "feature/brookesia-phone" ]; then
    echo "❌ 当前分支不是 feature/brookesia-phone (当前: $BRANCH)"
    echo "   执行: git checkout feature/brookesia-phone"
    exit 1
fi

echo "=== 1. Fetch origin ==="
git fetch origin

echo ""
echo "=== 2. Merge origin/master ==="
ORIG_HEAD=$(git rev-parse HEAD)
git merge origin/master --no-commit --no-ff || true

echo ""
echo "=== 3. 解决已知冲突 ==="

# idf_component.yml — 始终保留 brookesia 版本（LVGL 9.5.0）
if git diff --name-only --diff-filter=U | grep -q "main/idf_component.yml"; then
    echo "   解决 main/idf_component.yml → 保留 brookesia 版本"
    git checkout --ours main/idf_component.yml
    git add main/idf_component.yml
fi

# CMakeLists.txt / Kconfig.projbuild / sdkconfig.defaults — 保留双方
for f in main/CMakeLists.txt main/Kconfig.projbuild sdkconfig.defaults sdkconfig.defaults.esp32s3; do
    if git diff --name-only --diff-filter=U | grep -q "$f"; then
        echo "   解决 $f → 保留双方（追加模式）"
        # 合并双方改动
        git diff --theirs "$f" > /tmp/theirs.patch 2>/dev/null || true
        git checkout --ours "$f"
        if [ -s /tmp/theirs.patch ]; then
            # 提取 theirs 的新增行（+ 开头，非 +++）
            grep '^+[^+]' /tmp/theirs.patch | sed 's/^+//' >> "$f"
        fi
        git add "$f"
    fi
done

echo ""
echo "=== 4. 接口变更检测 ==="
bash scripts/check_interface_changes.sh "$ORIG_HEAD" HEAD || true

echo ""
echo "=== 5. 剩余未解决冲突 ==="
REMAINING=$(git diff --name-only --diff-filter=U 2>/dev/null || true)
if [ -n "$REMAINING" ]; then
    echo "⚠️  以下文件仍有冲突，需手动解决："
    echo "$REMAINING"
else
    echo "✅ 无剩余冲突"
fi

echo ""
echo "=== 6. 下一步 ==="
echo "   • 检查接口变更检测结果"
echo "   • idf.py build 验证编译"
echo "   • 功能测试"
echo "   • git commit 完成合并"
