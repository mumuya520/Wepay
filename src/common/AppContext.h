// AppContext.h — 全局应用上下文（与启动目录无关的路径信息）
// 解决 config.json 与可执行文件相对位置不一致的问题。
#pragma once // 防止头文件重复包含
#include <filesystem> // 文件系统库
#include <string> // 字符串库

// 应用上下文类
class AppContext {
public:
    // 获取单例
    static AppContext &instance() { static AppContext c; return c; }

    // 配置文件所在目录（绝对路径，始终基于实际 config.json 位置）
    // 设置配置目录
    void setConfigDir(const std::filesystem::path &p) { configDir_ = std::filesystem::absolute(p); }
    // 获取配置目录
    const std::filesystem::path &configDir() const { return configDir_; }

    // config.json 的完整绝对路径
    // 设置配置文件路径
    void setConfigPath(const std::filesystem::path &p) { configPath_ = std::filesystem::absolute(p); }
    // 获取配置文件路径
    const std::filesystem::path &configPath() const { return configPath_; }

private:
    AppContext() = default; // 私有构造函数
    std::filesystem::path configDir_;   // 配置目录（e.g. G:/back/recovered/wepay-cpp）
    std::filesystem::path configPath_;   // 配置文件路径（e.g. G:/back/recovered/wepay-cpp/config.json）
};
