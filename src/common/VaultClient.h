// WePay-Cpp — Vault 密钥管理客户端
// VaultClient.h — HashiCorp Vault KV-v2 HTTP 客户端（基于 SyncHttp，header-only）
// 用于从 Vault 读写敏感配置（通道密钥、支付证书路径等）
// Vault 地址: http://127.0.0.1:8200
// 挂载路径:  secret/ (KV v2)
// 用法:
//   auto val = VaultClient::instance().get("channel/wxpay", "appkey");
//   VaultClient::instance().put("channel/wxpay", {{"appkey","xxx"},{"appsecret","yyy"}});
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <map> // 映射容器
#include <vector>
#include <json/json.h>
#include <trantor/utils/Logger.h>
#include "SyncHttp.h"

// Vault 密钥管理客户端类
// 职责：
//   1. 连接 HashiCorp Vault 密钥管理系统
//   2. 支持 KV-v2 密钥引擎的读写操作
//   3. 用于存储敏感配置（通道密钥、支付证书路径等）
//   4. 支持自动降级（Vault 不可用时返回空）
class VaultClient {
public:
    // Vault 配置结构体
    struct Config {
        // 是否启用 Vault
        bool        enabled  = false;
        // Vault 服务器地址（默认 http://127.0.0.1:8200）
        std::string addr     = "http://127.0.0.1:8200";
        // Vault 认证令牌（Root token 或 AppRole token）
        std::string token;
        // KV-v2 引擎挂载点（默认 secret）
        std::string mount    = "secret";
    };

    // 获取单例实例
    static VaultClient& instance() {
        // 静态单例
        static VaultClient inst;
        return inst;
    }

    // 配置 Vault 客户端
    // 参数 appCfg：应用配置对象（JSON）
    // 配置项：
    //   vault.enabled - 启用 Vault（默认 false）
    //   vault.addr - Vault 服务器地址（默认 http://127.0.0.1:8200）
    //   vault.token - Vault 认证令牌
    //   vault.mount - KV-v2 挂载点（默认 secret）
    void configure(const Json::Value& appCfg) {
        // 检查是否有 vault 配置
        if (!appCfg.isMember("vault"))
            return;
        // 获取 vault 配置
        const auto& v = appCfg["vault"];
        // 设置启用标志
        cfg_.enabled = v.get("enabled", false).asBool();
        // 设置服务器地址
        cfg_.addr    = v.get("addr",    "http://127.0.0.1:8200").asString();
        // 设置认证令牌
        cfg_.token   = v.get("token",   "").asString();
        // 设置挂载点
        cfg_.mount   = v.get("mount",   "secret").asString();
        // 如果启用，记录日志
        if (cfg_.enabled)
            LOG_INFO << "[VaultClient] 已配置 addr=" << cfg_.addr
                     << " mount=" << cfg_.mount;
    }

    // 检查 Vault 是否启用且有有效令牌
    // 返回：是否启用且配置完整
    bool isEnabled() const {
        return cfg_.enabled && !cfg_.token.empty();
    }

    // 探测 Vault 是否就绪
    // 返回：Vault 是否可用（已初始化未封印或 standby）
    // 说明：
    //   - 200 = 已初始化未封印（可用）
    //   - 429 = Standby 节点（可用）
    //   - 其他 = 不可用
    bool isReady() const {
        // 检查是否启用
        if (!isEnabled())
            return false;
        // 调用 Vault health 端点
        auto r = SyncHttp::get(cfg_.addr + "/v1/sys/health", authHeaders(), 3);
        // 检查响应状态（200 或 429 均视为可用）
        return r.success && (r.status == 200 || r.status == 429);
    }

    // ── KV-v2：读单个字段 ──────────────────────────────────────────────────
    // 参数 path：密钥路径（不含 mount，如 "channel/wxpay"）
    // 参数 field：字段名（为空时返回所有字段的 JSON 字符串）
    // 返回：字段值（不存在或出错返回空字符串）
    std::string get(const std::string& path, const std::string& field = "") const {
        // 检查是否启用
        if (!isEnabled())
            return "";
        // 构建 Vault API URL
        std::string url = cfg_.addr + "/v1/" + cfg_.mount + "/data/" + path;
        // 发送 GET 请求
        auto r = SyncHttp::get(url, authHeaders(), 5);
        // 检查响应是否成功
        if (!r.success || r.status != 200) {
            // 记录警告日志
            LOG_WARN << "[VaultClient] GET " << path << " status=" << r.status;
            return "";
        }
        // 解析 JSON 响应
        Json::Value root;
        Json::Reader rd;
        // 检查解析是否成功
        if (!rd.parse(r.body, root))
            return "";
        // 获取数据字段（Vault KV-v2 的数据在 data.data 中）
        const Json::Value& data = root["data"]["data"];
        // 如果字段为空，返回所有字段的 JSON 字符串
        if (field.empty()) {
            Json::FastWriter fw;
            return fw.write(data);
        }
        // 检查字段是否存在
        if (!data.isMember(field))
            return "";
        // 返回字段值
        return data[field].asString();
    }

    // ── KV-v2：读所有字段为 map ────────────────────────────────────────────
    // 参数 path：密钥路径
    // 返回：字段名→字段值的映射（不存在或出错返回空 map）
    std::map<std::string, std::string> getAll(const std::string& path) const {
        // 创建结果 map
        std::map<std::string, std::string> result;
        // 检查是否启用
        if (!isEnabled())
            return result;
        // 构建 Vault API URL
        std::string url = cfg_.addr + "/v1/" + cfg_.mount + "/data/" + path;
        // 发送 GET 请求
        auto r = SyncHttp::get(url, authHeaders(), 5);
        // 检查响应是否成功
        if (!r.success || r.status != 200)
            return result;
        // 解析 JSON 响应
        Json::Value root;
        Json::Reader rd;
        // 检查解析是否成功
        if (!rd.parse(r.body, root))
            return result;
        // 获取数据字段
        const Json::Value& data = root["data"]["data"];
        // 遍历所有字段
        for (auto& k : data.getMemberNames())
            result[k] = data[k].asString();
        // 返回结果
        return result;
    }

    // ── KV-v2：写入密钥（整体替换）────────────────────────────────────────
    // 参数 path：密钥路径
    // 参数 fields：字段名→字段值的映射
    // 返回：是否写入成功
    // 说明：此操作会整体替换密钥的所有字段
    bool put(const std::string& path,
             const std::map<std::string, std::string>& fields) {
        // 检查是否启用
        if (!isEnabled())
            return false;
        // 创建内层数据对象
        Json::Value inner;
        // 遍历所有字段
        for (auto& [k, v] : fields)
            inner[k] = v;
        // 创建请求体（Vault KV-v2 的数据在 data 中）
        Json::Value body;
        body["data"] = inner;
        // 序列化 JSON
        Json::FastWriter fw;
        // 构建 Vault API URL
        std::string url = cfg_.addr + "/v1/" + cfg_.mount + "/data/" + path;
        // 发送 POST 请求
        auto r = SyncHttp::postJson(url, fw.write(body), authHeaders(), 5);
        // 检查响应是否成功（200 或 204 均为成功）
        if (!r.success || (r.status != 200 && r.status != 204)) {
            // 记录警告日志
            LOG_WARN << "[VaultClient] PUT " << path << " status=" << r.status;
            return false;
        }
        return true;
    }

    // ── KV-v2：列出子路径 ──────────────────────────────────────────────────
    // 参数 path：密钥路径前缀（为空时列出根目录）
    // 返回：子路径列表
    std::vector<std::string> list(const std::string& path = "") const {
        // 创建结果向量
        std::vector<std::string> keys;
        // 检查是否启用
        if (!isEnabled())
            return keys;
        // 构建 Vault API URL（使用 metadata 端点）
        std::string url = cfg_.addr + "/v1/" + cfg_.mount + "/metadata/" + path;
        // 获取认证头
        auto headers = authHeaders();
        // 发送 LIST 请求（Vault 自定义 HTTP 方法）
        auto r = SyncHttp::doRequestPublic("LIST", url, "", headers, 5);
        // 检查响应是否成功
        if (!r.success || r.status != 200)
            return keys;
        // 解析 JSON 响应
        Json::Value root;
        Json::Reader rd;
        // 检查解析是否成功
        if (!rd.parse(r.body, root))
            return keys;
        // 遍历所有子路径
        for (auto& k : root["data"]["keys"])
            keys.push_back(k.asString());
        // 返回结果
        return keys;
    }

    // ── KV-v2：删除密钥 ────────────────────────────────────────────────────
    // 参数 path：密钥路径
    // 返回：是否删除成功
    // 说明：删除最新版本的密钥
    bool del(const std::string& path) {
        // 检查是否启用
        if (!isEnabled())
            return false;
        // 构建 Vault API URL（使用 metadata 端点）
        std::string url = cfg_.addr + "/v1/" + cfg_.mount + "/metadata/" + path;
        // 发送 DELETE 请求
        auto r = SyncHttp::doRequestPublic("DELETE", url, "", authHeaders(), 5);
        // 检查响应是否成功（204 No Content）
        return r.success && r.status == 204;
    }

    // ── 便捷方法：读取支付通道字段 ──────────────────────────────────────────
    // 参数 pluginCode：通道代码（如 wepay_wxpay_default）
    // 参数 field：字段名
    // 返回：字段值（不存在返回空字符串）
    // 说明：自动将通道代码转换为 Vault 路径 "channel/{code}"
    std::string getChannelField(const std::string& pluginCode,
                                const std::string& field) const {
        return get("channel/" + pluginCode, field);
    }

    // ── 便捷方法：读取支付通道所有参数 ──────────────────────────────────────
    // 参数 pluginCode：通道代码
    // 返回：参数名→参数值的映射
    std::map<std::string, std::string> getChannelParams(const std::string& pluginCode) const {
        return getAll("channel/" + pluginCode);
    }

    // 获取当前配置
    const Config& config() const {
        return cfg_;
    }

// 私有区域
private:
    // 私有构造函数（单例）
    VaultClient() = default;
    // 配置对象
    Config cfg_;

    // 构建认证请求头
    // 返回：包含 X-Vault-Token 的请求头 map
    std::map<std::string, std::string> authHeaders() const {
        return {{"X-Vault-Token", cfg_.token}};
    }
};
