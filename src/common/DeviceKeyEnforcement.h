// WePay-Cpp — 设备密钥强制绑定服务
// 用户首次密码登录后必须生成设备密钥，否则无法使用系统
// 但后续可以用三种方式登录：密码登录、EdDSA登录、RSA登录
//
// 生产环境：可以通过环境变量 ENABLE_DEVICE_KEY_CHECK=1 强制启用检查
// 开发环境（默认）：跳过设备密钥强制绑定，用户可直接使用系统
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <vector> // 向量容器
#include <map> // 映射容器
#include <cstdlib> // 标准库
#include "PayDb.h" // 数据库操作

// 设备密钥强制绑定服务类
class DeviceKeyEnforcement {
public:
    // 检查用户是否已绑定设备密钥方法
    static bool hasDeviceKey(int userType, int userId) { // userType 用户类型，userId 用户 ID
        auto &db = PayDb::instance();
        auto row = db.queryOne(
            "SELECT COUNT(*) as cnt FROM device_keys WHERE user_type=? AND user_id=? AND state=1",
            {std::to_string(userType), std::to_string(userId)});

        int count = 0;
        try { count = std::stoi(row["cnt"]); } catch (...) {}
        return count > 0;
    }

    // 检查是否强制启用设备密钥检查（默认跳过，方便开发/部署）
    static bool isDeviceKeyCheckDisabled() {
        const char* env = std::getenv("ENABLE_DEVICE_KEY_CHECK");
        // 未设置 ENABLE_DEVICE_KEY_CHECK=1 时默认跳过检查
        return !env || std::string(env) != "1";
    }

    // 检查用户是否需要强制绑定设备密钥
    // 返回 true 表示需要强制绑定（阻止访问）
    static bool requiresDeviceKeySetup(int userType, int userId, const std::string &currentPath) {
        // 默认跳过检查；设置了 ENABLE_DEVICE_KEY_CHECK=1 才强制启用
        if (isDeviceKeyCheckDisabled()) {
            return false;
        }

        // 白名单路径：这些接口不需要设备密钥
        static const std::vector<std::string> whitelist = {
            "/auth/login",              // 密码登录（三种登录方式之一）
            "/auth/logout",             // 登出接口
            "/auth/refresh",            // 刷新 token
            "/auth/challenge",          // 获取挑战值（设备密钥登录第一步）
            "/auth/login-device",       // 设备密钥登录（三种登录方式之二、之三）
            "/auth/register-device",    // 注册设备（首次绑定）
            "/auth/devices",            // 查看设备列表
            "/auth/device",             // 设备管理
            "/auth/device-key-status",  // 检查绑定状态
            "/sysuser/myInfo",          // 获取用户信息
            "/refund/",                 // 退款相关接口
            "/order/",                  // 订单相关接口
            "/merchant/",               // 商户相关接口
        };

        // 检查是否在白名单中
        for (const auto &path : whitelist) {
            if (currentPath.find(path) != std::string::npos) {
                return false;  // 白名单路径，不需要检查
            }
        }

        // 检查用户是否已绑定设备密钥
        return !hasDeviceKey(userType, userId);
    }

    // 获取用户的设备密钥绑定状态
    struct DeviceKeyStatus {
        bool hasDeviceKey;      // 是否已绑定设备密钥
        int deviceCount;        // 已绑定设备数量
        bool isRequired;        // 是否强制要求绑定
        std::string message;    // 提示信息
    };

    static DeviceKeyStatus getStatus(int userType, int userId) {
        auto &db = PayDb::instance();
        auto row = db.queryOne(
            "SELECT COUNT(*) as cnt FROM device_keys WHERE user_type=? AND user_id=? AND state=1",
            {std::to_string(userType), std::to_string(userId)});

        int count = 0;
        try { count = std::stoi(row["cnt"]); } catch (...) {}

        DeviceKeyStatus status;
        status.deviceCount = count;
        status.hasDeviceKey = (count > 0);
        status.isRequired = !isDeviceKeyCheckDisabled();  // 环境变量 ENABLE_DEVICE_KEY_CHECK=1 时强制

        if (status.hasDeviceKey) {
            status.message = "已绑定 " + std::to_string(count) + " 个设备，支持三种登录方式：密码登录、EdDSA设备密钥登录、RSA设备密钥登录";
        } else {
            status.message = "您还未绑定设备密钥，请先完成设备绑定才能使用系统。绑定后可使用三种方式登录：密码、EdDSA、RSA";
        }

        return status;
    }
};
