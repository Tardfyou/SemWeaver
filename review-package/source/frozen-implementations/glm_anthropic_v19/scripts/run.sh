#!/bin/bash
# SemWeaver convenience wrapper

cd "$(dirname "$0")/.."

# 默认参数
PATCH_FILE="${1:-tests/tiny_buffer_lab/patches/tiny_copy_bounds_fix.patch}"
OUTPUT_DIR="${2:-output}"

echo "=========================================="
echo "SemWeaver"
echo "=========================================="
echo "补丁文件: $PATCH_FILE"
echo "输出目录: $OUTPUT_DIR"
echo "=========================================="

python3 -m src.main generate --patch "$PATCH_FILE" --output "$OUTPUT_DIR"
