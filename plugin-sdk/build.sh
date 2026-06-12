#!/bin/bash
# WePay 插件一键编译脚本 (Linux/macOS)
# 用法: ./build.sh [插件名]
# 示例: ./build.sh demo

set -e

PLUGIN_NAME=${1:-"demo"}

echo "=== 编译插件: $PLUGIN_NAME ==="

# 检查源文件
if [ ! -f "${PLUGIN_NAME}_plugin.cpp" ]; then
    echo "[错误] 未找到 ${PLUGIN_NAME}_plugin.cpp"
    exit 1
fi

if [ ! -f "plugin.json" ]; then
    echo "[错误] 未找到 plugin.json"
    exit 1
fi

# 创建构建目录
mkdir -p build && cd build

# CMake 配置
cmake .. \
    -DPLUGIN_NAME="$PLUGIN_NAME" \
    -DCMAKE_BUILD_TYPE=Release

# 编译
make -j$(nproc)

echo ""
echo "=== 打包为 zip ==="
make package

echo ""
echo "=== 完成 ==="
echo "输出文件: build/${PLUGIN_NAME}.zip"
echo "在管理后台 → 插件管理 → 导入插件 上传即可"
