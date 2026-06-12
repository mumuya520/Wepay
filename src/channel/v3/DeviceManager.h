#pragma once
#include <string>
#include <memory>
#include <redis++/redis++.h>
#include "nlohmann/json.hpp"

namespace wepay {
namespace v3 {

// 设备信息
struct DeviceInfo {
    std::string deviceId;
    std::string ip;
    int64_t lastHeartbeat = 0;
    bool online = false;
    int battery = 0;              // 电量百分比
    std::string network;          // 网络类型：WiFi/4G/5G
    std::string appVersion;       // App版本
    std::string screenResolution; // 屏幕分辨率
    std::string pid;              // 商户号（mch_no）
    std::vector<std::string> merchantIds; // 绑定的商户列表
};

// 设备管理器
class DeviceManager {
public:
    DeviceManager(std::shared_ptr<sw::redis::Redis> redis);

    // 注册设备
    bool registerDevice(const std::string& deviceId, const std::string& merchantId);

    // 更新心跳
    bool updateHeartbeat(const std::string& deviceId, const DeviceInfo& info);

    // 获取设备信息
    std::optional<DeviceInfo> getDeviceInfo(const std::string& deviceId);

    // 检查设备是否在线
    bool isDeviceOnline(const std::string& deviceId);

    // 获取商户的所有在线设备
    std::vector<std::string> getOnlineDevices(const std::string& merchantId);

    // 设备离线检测（定时任务调用）
    std::vector<std::string> checkOfflineDevices(int timeoutSeconds = 180);

    // 绑定设备到商户
    bool bindDeviceToMerchant(const std::string& deviceId, const std::string& merchantId);

    // 解绑设备
    bool unbindDevice(const std::string& deviceId, const std::string& merchantId);

    // 获取设备数量
    int getDeviceCount(const std::string& merchantId = "");

private:
    std::shared_ptr<sw::redis::Redis> redis_;

    std::string getDeviceKey(const std::string& deviceId);
    std::string getMerchantDevicesKey(const std::string& merchantId);
};

} // namespace v3
} // namespace wepay
