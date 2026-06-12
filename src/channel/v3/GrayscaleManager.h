#pragma once

#include <string>
#include <memory>
#include <vector>
#include <unordered_map>
#include <functional>
#include <sw/redis++/redis++.h>
#include <nlohmann/json.hpp>

namespace wepay {
namespace v3 {

/**
 * 灰度策略类型
 */
enum class GrayscaleStrategy {
    DEVICE_ID,      // 按设备ID灰度
    MERCHANT_ID,    // 按商户ID灰度
    PERCENTAGE,     // 按百分比灰度
    WHITELIST       // 白名单灰度
};

/**
 * 灰度规则
 */
struct GrayscaleRule {
    std::string featureName;                    // 功能名称
    GrayscaleStrategy strategy;                 // 灰度策略
    std::vector<std::string> targetList;        // 目标列表（设备ID/商户ID/白名单）
    int percentage;                             // 百分比（0-100）
    bool enabled;                               // 是否启用
    int64_t startTime;                          // 开始时间
    int64_t endTime;                            // 结束时间
};

/**
 * 灰度发布管理器
 */
class GrayscaleManager {
public:
    static GrayscaleManager& getInstance() {
        static GrayscaleManager instance;
        return instance;
    }

    // 初始化
    void init(std::shared_ptr<sw::redis::Redis> redis);

    // 注册灰度规则
    void registerRule(const GrayscaleRule& rule);

    // 移除灰度规则
    void removeRule(const std::string& featureName);

    // 检查是否命中灰度
    bool isGrayscaleHit(const std::string& featureName,
                       const std::string& deviceId,
                       const std::string& merchantId);

    // 从Redis加载规则
    void loadRulesFromRedis();

    // 保存规则到Redis
    void saveRulesToRedis();

    // 获取所有规则
    std::vector<GrayscaleRule> getAllRules() const;

    // 获取灰度统计
    struct GrayscaleStats {
        std::string featureName;
        int64_t totalRequests;
        int64_t grayscaleHits;
        double hitRate;
    };
    GrayscaleStats getStats(const std::string& featureName);

private:
    GrayscaleManager() = default;
    ~GrayscaleManager() = default;

    std::shared_ptr<sw::redis::Redis> redis_;
    std::unordered_map<std::string, GrayscaleRule> rules_;
    mutable std::mutex mutex_;

    // 检查设备ID是否命中
    bool checkDeviceId(const GrayscaleRule& rule, const std::string& deviceId);

    // 检查商户ID是否命中
    bool checkMerchantId(const GrayscaleRule& rule, const std::string& merchantId);

    // 检查百分比是否命中
    bool checkPercentage(const GrayscaleRule& rule, const std::string& key);

    // 检查白名单是否命中
    bool checkWhitelist(const GrayscaleRule& rule, const std::string& key);

    // 记录灰度命中
    void recordHit(const std::string& featureName, bool hit);

    // 计算哈希值（用于百分比灰度）
    uint32_t calculateHash(const std::string& key) const;
};

/**
 * 设备分组类型
 */
enum class DeviceGroupType {
    REGION,         // 按地区分组
    MERCHANT,       // 按商户分组
    VERSION,        // 按版本分组
    CUSTOM          // 自定义分组
};

/**
 * 设备分组
 */
struct DeviceGroup {
    std::string groupId;                        // 分组ID
    std::string groupName;                      // 分组名称
    DeviceGroupType type;                       // 分组类型
    std::vector<std::string> deviceIds;         // 设备ID列表
    nlohmann::json metadata;                    // 元数据
    int64_t createTime;                         // 创建时间
    int64_t updateTime;                         // 更新时间
};

/**
 * 设备分组管理器
 */
class DeviceGroupManager {
public:
    static DeviceGroupManager& getInstance() {
        static DeviceGroupManager instance;
        return instance;
    }

    // 初始化
    void init(std::shared_ptr<sw::redis::Redis> redis);

    // 创建分组
    bool createGroup(const DeviceGroup& group);

    // 删除分组
    bool deleteGroup(const std::string& groupId);

    // 更新分组
    bool updateGroup(const DeviceGroup& group);

    // 获取分组
    std::optional<DeviceGroup> getGroup(const std::string& groupId);

    // 获取所有分组
    std::vector<DeviceGroup> getAllGroups();

    // 添加设备到分组
    bool addDeviceToGroup(const std::string& groupId, const std::string& deviceId);

    // 从分组移除设备
    bool removeDeviceFromGroup(const std::string& groupId, const std::string& deviceId);

    // 获取设备所属分组
    std::vector<std::string> getDeviceGroups(const std::string& deviceId);

    // 获取分组内的所有设备
    std::vector<std::string> getGroupDevices(const std::string& groupId);

    // 按类型获取分组
    std::vector<DeviceGroup> getGroupsByType(DeviceGroupType type);

    // 批量操作
    bool batchAddDevices(const std::string& groupId, const std::vector<std::string>& deviceIds);
    bool batchRemoveDevices(const std::string& groupId, const std::vector<std::string>& deviceIds);

    // 分组统计
    struct GroupStats {
        std::string groupId;
        std::string groupName;
        int deviceCount;
        int onlineCount;
        int offlineCount;
    };
    GroupStats getGroupStats(const std::string& groupId);

private:
    DeviceGroupManager() = default;
    ~DeviceGroupManager() = default;

    std::shared_ptr<sw::redis::Redis> redis_;
    mutable std::mutex mutex_;

    static constexpr const char* REDIS_KEY_PREFIX = "device:group:";
    static constexpr const char* REDIS_DEVICE_GROUPS_PREFIX = "device:groups:";

    std::string buildGroupKey(const std::string& groupId) const;
    std::string buildDeviceGroupsKey(const std::string& deviceId) const;
};

// 便捷宏定义
#define GRAYSCALE_CHECK(feature, deviceId, merchantId) \
    wepay::v3::GrayscaleManager::getInstance().isGrayscaleHit(feature, deviceId, merchantId)

#define GRAYSCALE_ENABLED(feature, deviceId, merchantId) \
    if (!GRAYSCALE_CHECK(feature, deviceId, merchantId)) { \
        LOG_DEBUG << "Feature not enabled for grayscale: " << feature; \
        return; \
    }

} // namespace v3
} // namespace wepay
