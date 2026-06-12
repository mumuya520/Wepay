#include "DeviceManager.h"
#include <ctime>

namespace wepay {
namespace v3 {

DeviceManager::DeviceManager(std::shared_ptr<sw::redis::Redis> redis)
    : redis_(redis) {}

bool DeviceManager::registerDevice(const std::string& deviceId, const std::string& merchantId) {
    try {
        std::string key = getDeviceKey(deviceId);

        // 检查设备是否已存在
        if (redis_->exists(key)) {
            return false; // 设备已注册
        }

        // 创建设备信息
        DeviceInfo info;
        info.deviceId = deviceId;
        info.lastHeartbeat = std::time(nullptr);
        info.online = true;
        info.merchantIds.push_back(merchantId);

        // 存储到Redis
        nlohmann::json j;
        j["deviceId"] = info.deviceId;
        j["lastHeartbeat"] = info.lastHeartbeat;
        j["online"] = info.online;
        j["merchantIds"] = info.merchantIds;

        redis_->set(key, j.dump());

        // 添加到商户设备集合
        std::string merchantKey = getMerchantDevicesKey(merchantId);
        redis_->sadd(merchantKey, deviceId);

        return true;
    } catch (...) {
        return false;
    }
}

bool DeviceManager::updateHeartbeat(const std::string& deviceId, const DeviceInfo& info) {
    try {
        std::string key = getDeviceKey(deviceId);

        // 构建JSON
        nlohmann::json j;
        j["deviceId"] = deviceId;
        j["ip"] = info.ip;
        j["lastHeartbeat"] = std::time(nullptr);
        j["online"] = true;
        j["battery"] = info.battery;
        j["network"] = info.network;
        j["appVersion"] = info.appVersion;
        j["screenResolution"] = info.screenResolution;
        j["merchantIds"] = info.merchantIds;

        redis_->set(key, j.dump());

        return true;
    } catch (...) {
        return false;
    }
}

std::optional<DeviceInfo> DeviceManager::getDeviceInfo(const std::string& deviceId) {
    try {
        std::string key = getDeviceKey(deviceId);
        auto value = redis_->get(key);

        if (!value) {
            return std::nullopt;
        }

        nlohmann::json j = nlohmann::json::parse(*value);

        DeviceInfo info;
        info.deviceId = j["deviceId"];
        info.ip = j.value("ip", "");
        info.lastHeartbeat = j["lastHeartbeat"];
        info.online = j["online"];
        info.battery = j.value("battery", 0);
        info.network = j.value("network", "");
        info.appVersion = j.value("appVersion", "");
        info.screenResolution = j.value("screenResolution", "");

        if (j.contains("merchantIds")) {
            info.merchantIds = j["merchantIds"].get<std::vector<std::string>>();
        }

        return info;
    } catch (...) {
        return std::nullopt;
    }
}

bool DeviceManager::isDeviceOnline(const std::string& deviceId) {
    auto info = getDeviceInfo(deviceId);
    if (!info) return false;

    int64_t now = std::time(nullptr);
    return info->online && (now - info->lastHeartbeat) < 180;
}

std::vector<std::string> DeviceManager::getOnlineDevices(const std::string& merchantId) {
    std::vector<std::string> onlineDevices;

    try {
        std::string merchantKey = getMerchantDevicesKey(merchantId);
        std::vector<std::string> deviceIds;
        redis_->smembers(merchantKey, std::back_inserter(deviceIds));

        for (const auto& deviceId : deviceIds) {
            if (isDeviceOnline(deviceId)) {
                onlineDevices.push_back(deviceId);
            }
        }
    } catch (...) {
    }

    return onlineDevices;
}

std::vector<std::string> DeviceManager::checkOfflineDevices(int timeoutSeconds) {
    std::vector<std::string> offlineDevices;

    try {
        // 扫描所有设备key
        std::string pattern = "wepay:v3:device:*";
        sw::redis::Cursor cursor = 0;

        do {
            std::vector<std::string> keys;
            cursor = redis_->scan(cursor, pattern, 100, std::back_inserter(keys));

            for (const auto& key : keys) {
                std::string deviceId = key.substr(std::string("wepay:v3:device:").length());

                auto info = getDeviceInfo(deviceId);
                if (info) {
                    int64_t now = std::time(nullptr);
                    if ((now - info->lastHeartbeat) > timeoutSeconds) {
                        // 设备离线
                        offlineDevices.push_back(deviceId);

                        // 更新在线状态
                        info->online = false;
                        updateHeartbeat(deviceId, *info);
                    }
                }
            }
        } while (cursor != 0);

    } catch (...) {
    }

    return offlineDevices;
}

bool DeviceManager::bindDeviceToMerchant(const std::string& deviceId, const std::string& merchantId) {
    try {
        auto info = getDeviceInfo(deviceId);
        if (!info) return false;

        // 检查是否已绑定
        if (std::find(info->merchantIds.begin(), info->merchantIds.end(), merchantId)
            != info->merchantIds.end()) {
            return true; // 已绑定
        }

        // 添加商户ID
        info->merchantIds.push_back(merchantId);
        updateHeartbeat(deviceId, *info);

        // 添加到商户设备集合
        std::string merchantKey = getMerchantDevicesKey(merchantId);
        redis_->sadd(merchantKey, deviceId);

        return true;
    } catch (...) {
        return false;
    }
}

bool DeviceManager::unbindDevice(const std::string& deviceId, const std::string& merchantId) {
    try {
        auto info = getDeviceInfo(deviceId);
        if (!info) return false;

        // 移除商户ID
        auto it = std::find(info->merchantIds.begin(), info->merchantIds.end(), merchantId);
        if (it != info->merchantIds.end()) {
            info->merchantIds.erase(it);
            updateHeartbeat(deviceId, *info);
        }

        // 从商户设备集合移除
        std::string merchantKey = getMerchantDevicesKey(merchantId);
        redis_->srem(merchantKey, deviceId);

        return true;
    } catch (...) {
        return false;
    }
}

int DeviceManager::getDeviceCount(const std::string& merchantId) {
    try {
        if (merchantId.empty()) {
            // 统计所有设备
            std::string pattern = "wepay:v3:device:*";
            sw::redis::Cursor cursor = 0;
            int count = 0;

            do {
                std::vector<std::string> keys;
                cursor = redis_->scan(cursor, pattern, 100, std::back_inserter(keys));
                count += (int)keys.size();
            } while (cursor != 0);

            return count;
        } else {
            // 统计指定商户的设备
            std::string merchantKey = getMerchantDevicesKey(merchantId);
            return redis_->scard(merchantKey);
        }
    } catch (...) {
        return 0;
    }
}

std::string DeviceManager::getDeviceKey(const std::string& deviceId) {
    return "wepay:v3:device:" + deviceId;
}

std::string DeviceManager::getMerchantDevicesKey(const std::string& merchantId) {
    return "wepay:v3:merchant:" + merchantId + ":devices";
}

} // namespace v3
} // namespace wepay
