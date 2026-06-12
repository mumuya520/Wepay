#include "GrayscaleManager.h"
#include <drogon/drogon.h>
#include <algorithm>
#include <random>

namespace wepay {
namespace v3 {

// GrayscaleManager实现
void GrayscaleManager::init(std::shared_ptr<sw::redis::Redis> redis) {
    redis_ = redis;
    loadRulesFromRedis();
    LOG_INFO << "GrayscaleManager initialized with " << rules_.size() << " rules";
}

void GrayscaleManager::registerRule(const GrayscaleRule& rule) {
    std::lock_guard<std::mutex> lock(mutex_);
    rules_[rule.featureName] = rule;
    saveRulesToRedis();
    LOG_INFO << "Grayscale rule registered: " << rule.featureName;
}

void GrayscaleManager::removeRule(const std::string& featureName) {
    std::lock_guard<std::mutex> lock(mutex_);
    rules_.erase(featureName);
    saveRulesToRedis();
    LOG_INFO << "Grayscale rule removed: " << featureName;
}

bool GrayscaleManager::isGrayscaleHit(const std::string& featureName,
                                      const std::string& deviceId,
                                      const std::string& merchantId) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = rules_.find(featureName);
    if (it == rules_.end()) {
        // 没有灰度规则，默认全量放开
        return true;
    }

    const auto& rule = it->second;

    // 检查是否启用
    if (!rule.enabled) {
        return false;
    }

    // 检查时间范围
    int64_t now = std::time(nullptr);
    if (rule.startTime > 0 && now < rule.startTime) {
        return false;
    }
    if (rule.endTime > 0 && now > rule.endTime) {
        return false;
    }

    bool hit = false;

    // 根据策略判断
    switch (rule.strategy) {
        case GrayscaleStrategy::DEVICE_ID:
            hit = checkDeviceId(rule, deviceId);
            break;
        case GrayscaleStrategy::MERCHANT_ID:
            hit = checkMerchantId(rule, merchantId);
            break;
        case GrayscaleStrategy::PERCENTAGE:
            hit = checkPercentage(rule, deviceId + merchantId);
            break;
        case GrayscaleStrategy::WHITELIST:
            hit = checkWhitelist(rule, deviceId);
            break;
    }

    recordHit(featureName, hit);
    return hit;
}

bool GrayscaleManager::checkDeviceId(const GrayscaleRule& rule,
                                     const std::string& deviceId) {
    return std::find(rule.targetList.begin(), rule.targetList.end(), deviceId)
           != rule.targetList.end();
}

bool GrayscaleManager::checkMerchantId(const GrayscaleRule& rule,
                                       const std::string& merchantId) {
    return std::find(rule.targetList.begin(), rule.targetList.end(), merchantId)
           != rule.targetList.end();
}

bool GrayscaleManager::checkPercentage(const GrayscaleRule& rule,
                                       const std::string& key) {
    uint32_t hash = calculateHash(key);
    return (hash % 100) < static_cast<uint32_t>(rule.percentage);
}

bool GrayscaleManager::checkWhitelist(const GrayscaleRule& rule,
                                      const std::string& key) {
    return std::find(rule.targetList.begin(), rule.targetList.end(), key)
           != rule.targetList.end();
}

uint32_t GrayscaleManager::calculateHash(const std::string& key) const {
    // 使用简单的哈希算法
    uint32_t hash = 0;
    for (char c : key) {
        hash = hash * 31 + static_cast<uint32_t>(c);
    }
    return hash;
}

void GrayscaleManager::recordHit(const std::string& featureName, bool hit) {
    try {
        std::string totalKey = "grayscale:stats:" + featureName + ":total";
        std::string hitKey = "grayscale:stats:" + featureName + ":hits";

        redis_->incr(totalKey);
        if (hit) {
            redis_->incr(hitKey);
        }
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to record grayscale hit: " << e.what();
    }
}

void GrayscaleManager::loadRulesFromRedis() {
    if (!redis_) return;
    try {
        std::string key = "grayscale:rules";
        auto value = redis_->get(key);

        if (value) {
            nlohmann::json j = nlohmann::json::parse(*value);

            for (const auto& item : j) {
                GrayscaleRule rule;
                rule.featureName = item["featureName"].get<std::string>();
                rule.strategy = static_cast<GrayscaleStrategy>(item["strategy"].get<int>());
                rule.targetList = item["targetList"].get<std::vector<std::string>>();
                rule.percentage = item["percentage"].get<int>();
                rule.enabled = item["enabled"].get<bool>();
                rule.startTime = item["startTime"].get<int64_t>();
                rule.endTime = item["endTime"].get<int64_t>();

                rules_[rule.featureName] = rule;
            }

            LOG_INFO << "Loaded " << rules_.size() << " grayscale rules from Redis";
        }
    } catch (const std::exception& e) {
        LOG_WARN << "[V3-Grayscale] Redis unavailable, running without grayscale rules (all features open): " << e.what();
    }
}

void GrayscaleManager::saveRulesToRedis() {
    if (!redis_) return;
    try {
        nlohmann::json j = nlohmann::json::array();

        for (const auto& [name, rule] : rules_) {
            nlohmann::json item;
            item["featureName"] = rule.featureName;
            item["strategy"] = static_cast<int>(rule.strategy);
            item["targetList"] = rule.targetList;
            item["percentage"] = rule.percentage;
            item["enabled"] = rule.enabled;
            item["startTime"] = rule.startTime;
            item["endTime"] = rule.endTime;

            j.push_back(item);
        }

        std::string key = "grayscale:rules";
        redis_->set(key, j.dump());

        LOG_INFO << "Saved " << rules_.size() << " grayscale rules to Redis";
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to save grayscale rules: " << e.what();
    }
}

std::vector<GrayscaleRule> GrayscaleManager::getAllRules() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<GrayscaleRule> result;
    for (const auto& [name, rule] : rules_) {
        result.push_back(rule);
    }
    return result;
}

GrayscaleManager::GrayscaleStats
GrayscaleManager::getStats(const std::string& featureName) {
    GrayscaleStats stats;
    stats.featureName = featureName;

    try {
        std::string totalKey = "grayscale:stats:" + featureName + ":total";
        std::string hitKey = "grayscale:stats:" + featureName + ":hits";

        auto totalVal = redis_->get(totalKey);
        auto hitVal = redis_->get(hitKey);

        stats.totalRequests = totalVal ? std::stoll(*totalVal) : 0;
        stats.grayscaleHits = hitVal ? std::stoll(*hitVal) : 0;

        if (stats.totalRequests > 0) {
            stats.hitRate = static_cast<double>(stats.grayscaleHits) / stats.totalRequests * 100.0;
        } else {
            stats.hitRate = 0.0;
        }
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to get grayscale stats: " << e.what();
    }

    return stats;
}

// DeviceGroupManager实现
void DeviceGroupManager::init(std::shared_ptr<sw::redis::Redis> redis) {
    redis_ = redis;
    LOG_INFO << "DeviceGroupManager initialized";
}

std::string DeviceGroupManager::buildGroupKey(const std::string& groupId) const {
    return std::string(REDIS_KEY_PREFIX) + groupId;
}

std::string DeviceGroupManager::buildDeviceGroupsKey(const std::string& deviceId) const {
    return std::string(REDIS_DEVICE_GROUPS_PREFIX) + deviceId;
}

bool DeviceGroupManager::createGroup(const DeviceGroup& group) {
    std::lock_guard<std::mutex> lock(mutex_);

    try {
        nlohmann::json j;
        j["groupId"] = group.groupId;
        j["groupName"] = group.groupName;
        j["type"] = static_cast<int>(group.type);
        j["deviceIds"] = group.deviceIds;
        j["metadata"] = group.metadata;
        j["createTime"] = group.createTime;
        j["updateTime"] = group.updateTime;

        std::string key = buildGroupKey(group.groupId);
        redis_->set(key, j.dump());

        // 更新设备的分组映射
        for (const auto& deviceId : group.deviceIds) {
            std::string deviceKey = buildDeviceGroupsKey(deviceId);
            redis_->sadd(deviceKey, group.groupId);
        }

        LOG_INFO << "Device group created: " << group.groupId;
        return true;

    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to create device group: " << e.what();
        return false;
    }
}

bool DeviceGroupManager::deleteGroup(const std::string& groupId) {
    std::lock_guard<std::mutex> lock(mutex_);

    try {
        auto group = getGroup(groupId);
        if (!group) {
            return false;
        }

        // 删除设备的分组映射
        for (const auto& deviceId : group->deviceIds) {
            std::string deviceKey = buildDeviceGroupsKey(deviceId);
            redis_->srem(deviceKey, groupId);
        }

        // 删除分组
        std::string key = buildGroupKey(groupId);
        redis_->del(key);

        LOG_INFO << "Device group deleted: " << groupId;
        return true;

    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to delete device group: " << e.what();
        return false;
    }
}

bool DeviceGroupManager::updateGroup(const DeviceGroup& group) {
    std::lock_guard<std::mutex> lock(mutex_);

    try {
        // 先获取旧的分组信息
        auto oldGroup = getGroup(group.groupId);
        if (!oldGroup) {
            return false;
        }

        // 更新分组信息
        nlohmann::json j;
        j["groupId"] = group.groupId;
        j["groupName"] = group.groupName;
        j["type"] = static_cast<int>(group.type);
        j["deviceIds"] = group.deviceIds;
        j["metadata"] = group.metadata;
        j["createTime"] = oldGroup->createTime;
        j["updateTime"] = std::time(nullptr);

        std::string key = buildGroupKey(group.groupId);
        redis_->set(key, j.dump());

        // 更新设备的分组映射
        // 移除旧设备
        for (const auto& deviceId : oldGroup->deviceIds) {
            if (std::find(group.deviceIds.begin(), group.deviceIds.end(), deviceId)
                == group.deviceIds.end()) {
                std::string deviceKey = buildDeviceGroupsKey(deviceId);
                redis_->srem(deviceKey, group.groupId);
            }
        }

        // 添加新设备
        for (const auto& deviceId : group.deviceIds) {
            if (std::find(oldGroup->deviceIds.begin(), oldGroup->deviceIds.end(), deviceId)
                == oldGroup->deviceIds.end()) {
                std::string deviceKey = buildDeviceGroupsKey(deviceId);
                redis_->sadd(deviceKey, group.groupId);
            }
        }

        LOG_INFO << "Device group updated: " << group.groupId;
        return true;

    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to update device group: " << e.what();
        return false;
    }
}

std::optional<DeviceGroup> DeviceGroupManager::getGroup(const std::string& groupId) {
    try {
        std::string key = buildGroupKey(groupId);
        auto value = redis_->get(key);

        if (!value) {
            return std::nullopt;
        }

        nlohmann::json j = nlohmann::json::parse(*value);

        DeviceGroup group;
        group.groupId = j["groupId"].get<std::string>();
        group.groupName = j["groupName"].get<std::string>();
        group.type = static_cast<DeviceGroupType>(j["type"].get<int>());
        group.deviceIds = j["deviceIds"].get<std::vector<std::string>>();
        group.metadata = j["metadata"];
        group.createTime = j["createTime"].get<int64_t>();
        group.updateTime = j["updateTime"].get<int64_t>();

        return group;

    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to get device group: " << e.what();
        return std::nullopt;
    }
}

std::vector<DeviceGroup> DeviceGroupManager::getAllGroups() {
    std::vector<DeviceGroup> groups;

    try {
        std::string pattern = std::string(REDIS_KEY_PREFIX) + "*";
        auto cursor = 0LL;
        std::vector<std::string> keys;

        do {
            cursor = redis_->scan(cursor, pattern, 100, std::back_inserter(keys));
        } while (cursor != 0);

        for (const auto& key : keys) {
            std::string groupId = key.substr(strlen(REDIS_KEY_PREFIX));
            auto group = getGroup(groupId);
            if (group) {
                groups.push_back(*group);
            }
        }

    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to get all device groups: " << e.what();
    }

    return groups;
}

bool DeviceGroupManager::addDeviceToGroup(const std::string& groupId,
                                          const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(mutex_);

    try {
        auto group = getGroup(groupId);
        if (!group) {
            return false;
        }

        // 添加设备到分组
        if (std::find(group->deviceIds.begin(), group->deviceIds.end(), deviceId)
            == group->deviceIds.end()) {
            group->deviceIds.push_back(deviceId);
            updateGroup(*group);

            std::string deviceKey = buildDeviceGroupsKey(deviceId);
            redis_->sadd(deviceKey, groupId);
        }

        return true;

    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to add device to group: " << e.what();
        return false;
    }
}

bool DeviceGroupManager::removeDeviceFromGroup(const std::string& groupId,
                                               const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(mutex_);

    try {
        auto group = getGroup(groupId);
        if (!group) {
            return false;
        }

        // 从分组移除设备
        auto it = std::find(group->deviceIds.begin(), group->deviceIds.end(), deviceId);
        if (it != group->deviceIds.end()) {
            group->deviceIds.erase(it);
            updateGroup(*group);

            std::string deviceKey = buildDeviceGroupsKey(deviceId);
            redis_->srem(deviceKey, groupId);
        }

        return true;

    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to remove device from group: " << e.what();
        return false;
    }
}

std::vector<std::string> DeviceGroupManager::getDeviceGroups(const std::string& deviceId) {
    std::vector<std::string> groups;

    try {
        std::string key = buildDeviceGroupsKey(deviceId);
        redis_->smembers(key, std::back_inserter(groups));
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to get device groups: " << e.what();
    }

    return groups;
}

std::vector<std::string> DeviceGroupManager::getGroupDevices(const std::string& groupId) {
    auto group = getGroup(groupId);
    if (group) {
        return group->deviceIds;
    }
    return {};
}

std::vector<DeviceGroup> DeviceGroupManager::getGroupsByType(DeviceGroupType type) {
    std::vector<DeviceGroup> result;
    auto allGroups = getAllGroups();

    for (const auto& group : allGroups) {
        if (group.type == type) {
            result.push_back(group);
        }
    }

    return result;
}

bool DeviceGroupManager::batchAddDevices(const std::string& groupId,
                                         const std::vector<std::string>& deviceIds) {
    for (const auto& deviceId : deviceIds) {
        if (!addDeviceToGroup(groupId, deviceId)) {
            return false;
        }
    }
    return true;
}

bool DeviceGroupManager::batchRemoveDevices(const std::string& groupId,
                                            const std::vector<std::string>& deviceIds) {
    for (const auto& deviceId : deviceIds) {
        if (!removeDeviceFromGroup(groupId, deviceId)) {
            return false;
        }
    }
    return true;
}

DeviceGroupManager::GroupStats
DeviceGroupManager::getGroupStats(const std::string& groupId) {
    GroupStats stats;
    stats.groupId = groupId;

    auto group = getGroup(groupId);
    if (!group) {
        stats.groupName = "";
        stats.deviceCount = 0;
        stats.onlineCount = 0;
        stats.offlineCount = 0;
        return stats;
    }

    stats.groupName = group->groupName;
    stats.deviceCount = group->deviceIds.size();

    // TODO: 查询设备在线状态
    stats.onlineCount = 0;
    stats.offlineCount = stats.deviceCount;

    return stats;
}

} // namespace v3
} // namespace wepay
