@echo off
:: WePay 插件一键编译脚本 (Windows)
:: 用法: build.bat [插件名]
:: 示例: build.bat demo
:: 需要: Visual Studio 2019+ 或 MinGW/MSYS2

set PLUGIN_NAME=%1
if "%PLUGIN_NAME%"=="" set PLUGIN_NAME=demo

echo === 编译插件: %PLUGIN_NAME% ===

if not exist "%PLUGIN_NAME%_plugin.cpp" (
    echo [错误] 未找到 %PLUGIN_NAME%_plugin.cpp
    exit /b 1
)

if not exist "plugin.json" (
    echo [错误] 未找到 plugin.json
    exit /b 1
)

:: 创建构建目录
if not exist build mkdir build
cd build

:: CMake 配置（优先用 MSYS2 MinGW）
cmake .. -DPLUGIN_NAME=%PLUGIN_NAME% -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo [错误] CMake 配置失败，请确认已安装 CMake 和编译器
    exit /b 1
)

:: 编译
cmake --build . --config Release
if errorlevel 1 (
    echo [错误] 编译失败
    exit /b 1
)

:: 打包
cmake --build . --target package --config Release

echo.
echo === 完成 ===
echo 输出文件: build\%PLUGIN_NAME%.zip
echo 在管理后台 → 插件管理 → 导入插件 上传即可
