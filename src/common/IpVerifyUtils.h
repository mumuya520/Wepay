// WePay-Cpp — IP 验证工具
// 用于设备密钥登录时验证 IP 地址
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <regex> // 正则表达式库

// IP 验证工具类，用于设备密钥登录时的 IP 地址验证
class IpVerifyUtils {
public:
    // 检查是否为本地 IP 方法（本地 IP 不需要验证）
    // 参数 ip：要检查的 IP 地址
    // 返回：true 表示是本地 IP，false 表示是外网 IP
    static bool isLocalIp(const std::string &ip) {
        // 检查 IPv4 本地地址范围：127.0.0.1 - 127.255.255.255 (localhost)
        if (ip.find("127.") == 0)
            // 返回 true 表示是本地地址
            return true;

        // 检查 IPv6 本地地址：::1 (IPv6 localhost)
        if (ip == "::1" || ip == "::ffff:127.0.0.1")
            // 返回 true 表示是 IPv6 本地地址
            return true;

        // 检查本地主机名
        if (ip == "localhost")
            // 返回 true 表示是本地主机名
            return true;

        // 检查私有网络 A：192.168.0.0 - 192.168.255.255
        if (ip.find("192.168.") == 0)
            // 返回 true 表示是私有网络 A
            return true;

        // 检查私有网络 B：10.0.0.0 - 10.255.255.255
        if (ip.find("10.") == 0)
            // 返回 true 表示是私有网络 B
            return true;

        // 检查私有网络 C：172.16.0.0 - 172.31.255.255
        // 创建正则表达式用于匹配私有网络 C 范围
        std::regex privateB("^172\\.(1[6-9]|2[0-9]|3[0-1])\\...*");
        // 使用正则表达式匹配 IP 地址
        if (std::regex_match(ip, privateB))
            // 返回 true 表示匹配私有网络 C
            return true;

        // 如果都不匹配，返回 false 表示非本地 IP
        return false;
    }

    // 验证 IP 是否在可信列表中
    // 参数 currentIp：当前登录的 IP 地址
    // 参数 trustedIpsJson：可信 IP 列表（JSON 数组格式）
    // 返回：true 表示 IP 在可信列表中或列表为空，false 表示 IP 不在列表中
    static bool isIpInTrustedList(const std::string &currentIp, const std::string &trustedIpsJson) {
        // 检查可信列表是否为空
        if (trustedIpsJson.empty() || trustedIpsJson == "[]" || trustedIpsJson == "{}") {
            // 如果列表为空，表示不限制 IP，返回 true
            return true;
        }

        // 简单的 JSON 数组解析（格式：["ip1","ip2","ip3"]）
        // 注意：这是一个简单的字符串查找，生产环境建议使用 jsoncpp 解析
        // 在 JSON 字符串中查找当前 IP
        if (trustedIpsJson.find(currentIp) != std::string::npos) {
            // 如果找到，返回 true 表示 IP 在可信列表中
            return true;
        }

        // 如果没有找到，返回 false 表示 IP 不在可信列表中
        return false;
    }

    // 格式化 IP 地址（移除 IPv6 前缀）
    // 参数 ip：要格式化的 IP 地址
    // 返回：格式化后的 IP 地址
    static std::string normalizeIp(const std::string &ip) {
        // 检查是否以 IPv6 映射前缀开头：::ffff:192.168.1.1 → 192.168.1.1
        if (ip.find("::ffff:") == 0) {
            // 移除前 7 个字符的前缀，返回剩余部分
            return ip.substr(7);
        }
        // 如果没有前缀，直接返回原 IP
        return ip;
    }

    // 验证设备密钥登录的 IP 地址
    // 参数 requireIpVerify：是否要求 IP 验证（从数据库读取）
    // 参数 trustedIps：可信 IP 列表（JSON 数组格式）
    // 参数 currentIp：当前登录的 IP 地址
    // 返回：pair<bool, string> - 第一个元素表示是否允许登录，第二个元素表示拒绝原因
    static std::pair<bool, std::string> verifyDeviceLoginIp(
        bool requireIpVerify,
        const std::string &trustedIps,
        const std::string &currentIp) {

        // 格式化当前 IP（移除 IPv6 前缀）
        std::string normalizedIp = normalizeIp(currentIp);

        // 本地 IP 始终允许（192.168.*/127.*/localhost）
        if (isLocalIp(normalizedIp)) {
            // 返回允许登录，原因为空
            return {true, ""};
        }

        // 如果不要求 IP 验证，直接通过
        if (!requireIpVerify) {
            // 返回允许登录，原因为空
            return {true, ""};
        }

        // 检查是否在可信 IP 列表中
        if (isIpInTrustedList(normalizedIp, trustedIps)) {
            // 返回允许登录，原因为空
            return {true, ""};
        }

        // IP 验证失败：IP 不在可信列表中
        // 返回拒绝登录，并提供详细的拒绝原因
        return {false, "当前 IP 地址 " + normalizedIp + " 不在可信列表中，请联系管理员添加"};
    }
// 类定义结束
};
