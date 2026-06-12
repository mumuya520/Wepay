// WePay-Cpp — Nacos 配置中心服务
#pragma once // 防止头文件重复包含

#ifdef WEPAY_HAS_NACOS // 可选的 Nacos 支持

#include <string> // 字符串库
#include <thread> // 线程库
#include <atomic> // 原子操作
#include <chrono> // 时间库
#include <sstream>
#include <drogon/drogon.h>

// 如果编译了官方 Nacos SDK
#ifdef WEPAY_NACOS_SDK
// ── Linux: 使用官方 nacos-sdk-cpp (.so) ──────────────────────
// 包含官方 Nacos SDK 头文件
#include "Nacos.h"
// 取消 NacosString.h 定义的宏，避免污染 Json::Value::isNull()
#ifdef isNull
#  undef isNull
#endif
// 使用 nacos 命名空间
using namespace nacos;

// Nacos 服务类（SDK 实现）
// 使用官方 nacos-sdk-cpp 库进行服务注册、发现和配置管理
// 支持服务注册、心跳、配置获取、服务注销等功能
class NacosService {
public:
    // 获取 Nacos 服务单例
    // 使用单例模式确保全局只有一个 Nacos 服务实例
    // 返回：全局唯一的 NacosService 实例引用
    static NacosService &instance() {
        // 使用静态变量实现单例模式（线程安全）
        static NacosService inst;
        // 返回实例引用
        return inst;
    }

    // 初始化 Nacos 服务
    // 向 Nacos 注册本服务实例，启用心跳保活
    // 参数 serverAddr：Nacos 服务器地址（如 "127.0.0.1:8848"）
    // 参数 serviceName：服务名称（如 "wepay-cpp"）
    // 参数 ip：本服务的 IP 地址
    // 参数 port：本服务的端口号
    // 参数 group：服务分组（默认 "DEFAULT_GROUP"）
    // 参数 ns：命名空间 ID（可选，用于多环境隔离）
    // 参数 heartbeatIntervalSec：心跳间隔（秒，SDK 自动处理）
    void init(const std::string &serverAddr,
              const std::string &serviceName,
              const std::string &ip,
              int port,
              const std::string &group = "DEFAULT_GROUP",
              const std::string &ns    = "",
              int /*heartbeatIntervalSec*/ = 5) {
        // 保存服务信息
        serviceName_ = serviceName;
        group_       = group;
        ip_          = ip;
        port_        = port;

        // 创建 Nacos 配置属性
        Properties props;
        // 设置 Nacos 服务器地址
        props[PropertyKeyConst::SERVER_ADDR] = serverAddr;
        // 如果指定了命名空间，设置命名空间 ID
        if (!ns.empty())
            props[PropertyKeyConst::NAMESPACE] = ns;

        // 尝试初始化 Nacos 服务
        try {
            // 创建 Nacos 工厂
            factory_   = NacosFactoryFactory::getNacosFactory(props);
            // 创建命名服务（用于服务注册和发现）
            namingSvc_ = factory_->CreateNamingService();
            // 创建配置服务（用于配置管理）
            configSvc_ = factory_->CreateConfigService();

            // 创建服务实例
            Instance inst;
            // 设置实例 IP
            inst.ip          = ip_;
            // 设置实例端口
            inst.port        = port_;
            // 设置集群名称
            inst.clusterName = "DEFAULT";
            // 设置为临时实例（心跳失败后自动删除）
            inst.ephemeral   = true;
            // 向 Nacos 注册服务实例
            namingSvc_->registerInstance(serviceName_, group_, inst);
            // 记录注册成功的日志
            LOG_INFO << "[Nacos] 注册成功(SDK): " << serviceName_
                     << " -> " << ip_ << ":" << port_;
        } catch (NacosException &e) {
            // 记录注册失败的日志
            LOG_ERROR << "[Nacos] SDK 注册失败: " << e.what();
        }
    }

    // 从 Nacos 获取配置
    // 用于动态获取配置中心的配置内容
    // 参数 dataId：配置 ID
    // 参数 grp：配置分组（默认 "DEFAULT_GROUP"）
    // 返回：配置内容（获取失败返回空字符串）
    std::string getConfig(const std::string &dataId,
                          const std::string &grp = "DEFAULT_GROUP") {
        // 检查配置服务是否初始化
        if (!configSvc_)
            return "";
        // 尝试获取配置
        try {
            // 从 Nacos 获取配置，超时时间 3000ms
            return configSvc_->getConfig(dataId, grp, 3000);
        } catch (NacosException &e) {
            // 记录获取失败的日志
            LOG_WARN << "[Nacos] getConfig 失败: " << e.what();
            // 返回空字符串
            return "";
        }
    }

    // 关闭 Nacos 服务
    // 注销服务实例，断开与 Nacos 的连接
    void shutdown() {
        // 检查命名服务是否初始化
        if (!namingSvc_)
            return;
        // 尝试注销服务实例
        try {
            // 从 Nacos 注销服务实例
            namingSvc_->deregisterInstance(serviceName_, group_, ip_, port_);
        } catch (...) {
            // 忽略注销失败的异常
        }
        // 记录注销成功的日志
        LOG_INFO << "[Nacos] 服务注销(SDK): " << serviceName_;
    }

    // 析构函数
    // 对象销毁时自动调用 shutdown 方法
    ~NacosService() {
        // 调用 shutdown 方法关闭服务
        shutdown();
    }

// 私有区域
private:
    // 构造函数（私有，禁止直接创建实例）
    // 强制使用 instance() 方法获取单例
    NacosService() = default;
    // 服务名称
    std::string           serviceName_;
    // 服务分组
    std::string           group_;
    // 服务 IP 地址
    std::string           ip_;
    // 服务端口号
    int                   port_ = 0;
    // Nacos 工厂
    INacosServiceFactory *factory_   = nullptr;
    // 命名服务（用于服务注册和发现）
    NamingService        *namingSvc_ = nullptr;
    // 配置服务（用于配置管理）
    ConfigService        *configSvc_ = nullptr;
// 类定义结束
};

#else
// ── Windows / 无 SDK: 纯 HTTP curl 实现 ──────────────────────
// 使用 libcurl 库通过 HTTP 协议与 Nacos 通信
#include <curl/curl.h>

// Nacos 服务类（HTTP 实现）
// 使用 libcurl 库通过 HTTP 协议与 Nacos 服务器通信
// 支持服务注册、心跳、配置获取、服务注销等功能
class NacosService {
public:
    // 获取 Nacos 服务单例
    // 使用单例模式确保全局只有一个 Nacos 服务实例
    // 返回：全局唯一的 NacosService 实例引用
    static NacosService &instance() {
        // 使用静态变量实现单例模式（线程安全）
        static NacosService inst;
        // 返回实例引用
        return inst;
    }

    // 初始化 Nacos 服务
    // 向 Nacos 注册本服务实例，启用心跳保活线程
    // 参数 serverAddr：Nacos 服务器地址（如 "127.0.0.1:8848"）
    // 参数 serviceName：服务名称（如 "wepay-cpp"）
    // 参数 ip：本服务的 IP 地址
    // 参数 port：本服务的端口号
    // 参数 group：服务分组（默认 "DEFAULT_GROUP"）
    // 参数 ns：命名空间 ID（可选，用于多环境隔离）
    // 参数 heartbeatIntervalSec：心跳间隔（秒）
    void init(const std::string &serverAddr,
              const std::string &serviceName,
              const std::string &ip,
              int port,
              const std::string &group     = "DEFAULT_GROUP",
              const std::string &ns        = "",
              int heartbeatIntervalSec     = 5) {
        // 保存服务信息
        serverAddr_   = serverAddr;
        serviceName_  = serviceName;
        ip_           = ip;
        port_         = port;
        group_        = group;
        ns_           = ns;

        // 尝试向 Nacos 注册服务
        if (doRegister()) {
            // 注册成功，记录日志
            LOG_INFO << "[Nacos] 注册成功: " << serviceName_
                     << " -> " << ip_ << ":" << port_;
            // 标记服务为运行状态
            running_ = true;
            // 启动心跳线程，定期发送心跳保活
            hbThread_ = std::thread([this, heartbeatIntervalSec]() {
                // 心跳循环
                while (running_) {
                    // 等待指定的心跳间隔
                    std::this_thread::sleep_for(
                        std::chrono::seconds(heartbeatIntervalSec));
                    // 如果服务仍在运行，发送心跳
                    if (running_)
                        sendHeartbeat();
                }
            });
        } else {
            // 注册失败，记录错误日志
            LOG_ERROR << "[Nacos] 注册失败，请检查 Nacos 地址: " << serverAddr_;
        }
    }

    // 从 Nacos 获取配置
    // 用于动态获取配置中心的配置内容
    // 参数 dataId：配置 ID
    // 参数 grp：配置分组（默认 "DEFAULT_GROUP"）
    // 返回：配置内容（获取失败返回空字符串）
    std::string getConfig(const std::string &dataId,
                          const std::string &grp = "DEFAULT_GROUP") {
        // 构建 Nacos 配置查询 URL
        std::string url = "http://" + serverAddr_
            + "/nacos/v1/cs/configs?dataId=" + urlEncode(dataId)
            + "&group=" + urlEncode(grp);
        // 如果指定了命名空间，添加到 URL
        if (!ns_.empty())
            url += "&tenant=" + urlEncode(ns_);
        // 发送 GET 请求获取配置
        return httpGet(url);
    }

    // 关闭 Nacos 服务
    // 停止心跳线程，注销服务实例
    void shutdown() {
        // 标记服务为停止状态
        running_ = false;
        // 等待心跳线程结束
        if (hbThread_.joinable())
            hbThread_.join();
        // 从 Nacos 注销服务实例
        doDeregister();
        // 记录注销成功的日志
        LOG_INFO << "[Nacos] 服务注销: " << serviceName_;
    }

    // 析构函数
    // 对象销毁时自动调用 shutdown 方法
    ~NacosService() {
        // 如果服务仍在运行，调用 shutdown 方法
        if (running_)
            shutdown();
    }

// 私有区域
private:
    // 构造函数（私有，禁止直接创建实例）
    // 强制使用 instance() 方法获取单例
    NacosService() = default;

    // Nacos 服务器地址
    std::string serverAddr_;
    // 服务名称
    std::string serviceName_;
    // 服务 IP 地址
    std::string ip_;
    // 服务端口号
    int         port_  = 0;
    // 服务分组
    std::string group_;
    // 命名空间 ID
    std::string ns_;
    // 服务运行状态标志
    std::atomic<bool>  running_{false};
    // 心跳线程
    std::thread        hbThread_;

    // HTTP 响应回调函数
    // 用于 curl 库接收 HTTP 响应数据
    // 参数 ptr：响应数据指针
    // 参数 size：数据块大小
    // 参数 nmemb：数据块数量
    // 参数 buf：接收缓冲区
    // 返回：接收的字节数
    static size_t writeCallback(char *ptr, size_t size, size_t nmemb, std::string *buf) {
        // 将响应数据追加到缓冲区
        buf->append(ptr, size * nmemb);
        // 返回接收的字节数
        return size * nmemb;
    }

    // 发送 HTTP GET 请求
    // 参数 url：请求 URL
    // 参数 timeoutMs：超时时间（毫秒，默认 3000ms）
    // 返回：响应内容（请求失败返回空字符串）
    std::string httpGet(const std::string &url, int timeoutMs = 3000) {
        // 初始化 curl 句柄
        CURL *c = curl_easy_init();
        // 如果初始化失败，返回空字符串
        if (!c)
            return "";
        // 响应缓冲区
        std::string resp;
        // 设置请求 URL
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        // 设置响应回调函数
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCallback);
        // 设置响应缓冲区
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
        // 设置超时时间
        curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)timeoutMs);
        // 禁用信号处理（避免多线程问题）
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        // 执行 HTTP 请求
        curl_easy_perform(c);
        // 清理 curl 句柄
        curl_easy_cleanup(c);
        // 返回响应内容
        return resp;
    }

    // 发送 HTTP POST 请求
    // 参数 url：请求 URL
    // 参数 body：请求体
    // 参数 timeoutMs：超时时间（毫秒，默认 3000ms）
    // 返回：请求是否成功
    bool httpPost(const std::string &url, const std::string &body, int timeoutMs = 3000) {
        // 初始化 curl 句柄
        CURL *c = curl_easy_init();
        // 如果初始化失败，返回 false
        if (!c)
            return false;
        // 响应缓冲区
        std::string resp;
        // 设置请求 URL
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        // 设置请求体
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
        // 设置响应回调函数
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCallback);
        // 设置响应缓冲区
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
        // 设置超时时间
        curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)timeoutMs);
        // 禁用信号处理（避免多线程问题）
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        // 执行 HTTP 请求
        CURLcode rc = curl_easy_perform(c);
        // 清理 curl 句柄
        curl_easy_cleanup(c);
        // 返回请求是否成功
        return rc == CURLE_OK;
    }

    // 向 Nacos 注册服务
    // 返回：注册是否成功
    bool doRegister() {
        // 构建注册 URL
        std::string url = "http://" + serverAddr_ + "/nacos/v1/ns/instance";
        // 构建请求体
        std::ostringstream body;
        body << "serviceName=" << urlEncode(serviceName_)
             << "&groupName="  << urlEncode(group_)
             << "&ip="         << urlEncode(ip_)
             << "&port="       << port_
             << "&healthy=true&ephemeral=true"
             << "&metadata=framework%3Ddrogon";
        // 如果指定了命名空间，添加到请求体
        if (!ns_.empty())
            body << "&namespaceId=" << urlEncode(ns_);
        // 发送 POST 请求注册服务
        return httpPost(url, body.str());
    }

    // 从 Nacos 注销服务
    void doDeregister() {
        // 构建注销 URL
        std::string url = "http://" + serverAddr_ + "/nacos/v1/ns/instance?"
            + "serviceName=" + urlEncode(serviceName_)
            + "&groupName="  + urlEncode(group_)
            + "&ip="         + urlEncode(ip_)
            + "&port="       + std::to_string(port_);
        // 如果指定了命名空间，添加到 URL
        if (!ns_.empty())
            url += "&namespaceId=" + urlEncode(ns_);
        // 初始化 curl 句柄
        CURL *c = curl_easy_init();
        // 如果初始化失败，直接返回
        if (!c)
            return;
        // 设置请求 URL
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        // 设置 HTTP 方法为 DELETE
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "DELETE");
        // 设置超时时间
        curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 3000L);
        // 禁用信号处理（避免多线程问题）
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        // 执行 HTTP 请求
        curl_easy_perform(c);
        // 清理 curl 句柄
        curl_easy_cleanup(c);
    }

    // 发送心跳保活
    // 定期调用此方法保持服务实例在 Nacos 中的活跃状态
    void sendHeartbeat() {
        // 构建心跳 JSON 对象
        // Nacos v1 心跳接口必须用 PUT 方法
        std::ostringstream beatJson;
        beatJson << "{\"serviceName\":\"" << serviceName_ << "\""
                 << ",\"groupName\":\""   << group_        << "\""
                 << ",\"ip\":\""          << ip_           << "\""
                 << ",\"port\":"          << port_
                 << ",\"weight\":1.0"
                 << ",\"ephemeral\":true}";
        // 构建查询字符串
        std::ostringstream qs;
        qs << "serviceName=" << urlEncode(serviceName_)
           << "&groupName="  << urlEncode(group_)
           << "&beat="       << urlEncode(beatJson.str());
        // 如果指定了命名空间，添加到查询字符串
        if (!ns_.empty())
            qs << "&namespaceId=" << urlEncode(ns_);
        // 构建完整的心跳 URL
        std::string url = "http://" + serverAddr_ + "/nacos/v1/ns/instance/beat?" + qs.str();
        // 发送 PUT 请求
        httpPut(url, 2000);
    }

    // 发送 HTTP PUT 请求
    // 参数 url：请求 URL
    // 参数 timeoutMs：超时时间（毫秒，默认 3000ms）
    // 返回：请求是否成功
    bool httpPut(const std::string &url, int timeoutMs = 3000) {
        // 初始化 curl 句柄
        CURL *c = curl_easy_init();
        // 如果初始化失败，返回 false
        if (!c)
            return false;
        // 响应缓冲区
        std::string resp;
        // 设置请求 URL
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        // 设置 HTTP 方法为 PUT
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "PUT");
        // 设置空请求体
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, "");
        // 设置响应回调函数
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCallback);
        // 设置响应缓冲区
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
        // 设置超时时间
        curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long)timeoutMs);
        // 禁用信号处理（避免多线程问题）
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        // 执行 HTTP 请求
        CURLcode rc = curl_easy_perform(c);
        // 清理 curl 句柄
        curl_easy_cleanup(c);
        // 返回请求是否成功
        return rc == CURLE_OK;
    }

    // URL 编码
    // 使用 curl 库的 URL 编码函数对字符串进行编码
    // 参数 s：待编码的字符串
    // 返回：编码后的字符串
    static std::string urlEncode(const std::string &s) {
        // 初始化 curl 句柄
        CURL *c = curl_easy_init();
        // 如果初始化失败，返回原字符串
        if (!c)
            return s;
        // 使用 curl 库进行 URL 编码
        char *enc = curl_easy_escape(c, s.c_str(), (int)s.size());
        // 将编码结果转换为字符串
        std::string result = enc ? enc : s;
        // 释放编码结果
        curl_free(enc);
        // 清理 curl 句柄
        curl_easy_cleanup(c);
        // 返回编码结果
        return result;
    }
};

#endif // WEPAY_NACOS_SDK
#endif // WEPAY_HAS_NACOS
