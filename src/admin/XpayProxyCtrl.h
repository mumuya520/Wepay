// WePay-Cpp — 管理后台: Xpay 反向代理控制器
// 职责：将所有 /xpay/** 请求转发给 xpay-go 内嵌 Gin engine（进程内 FFI HTTP 派发，零 TCP 开销）
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <string> // 字符串库
#include <map> // 映射库
#include "../common/XpayBridge.h" // Xpay 桥接

// Xpay 反向代理控制器类，继承自 Drogon HTTP 控制器
class XpayProxyCtrl : public drogon::HttpController<XpayProxyCtrl> {
public:
    // 开始注册 HTTP 方法列表
    METHOD_LIST_BEGIN
        // 注册 /xpay 路由，支持 GET、POST、PUT、DELETE、OPTIONS 方法，转发到 root() 处理
        ADD_METHOD_TO(XpayProxyCtrl::root,  "/xpay",  drogon::Get, drogon::Post, drogon::Put, drogon::Delete, drogon::Options);
        // 注册 /xpay/ 路由（带斜杠），支持 GET、POST、PUT、DELETE、OPTIONS 方法，转发到 root() 处理
        ADD_METHOD_TO(XpayProxyCtrl::root,  "/xpay/", drogon::Get, drogon::Post, drogon::Put, drogon::Delete, drogon::Options);
    // 结束方法列表注册
    METHOD_LIST_END

    // /xpay 和 /xpay/ 路由处理函数
    // 参数 req：HTTP 请求对象指针
    // 参数 cb：异步回调函数，用于返回 HTTP 响应
    void root(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        // 调用 forward() 方法转发请求到 xpay-go，目标路径为 "/"
        forward(req, "/", std::move(cb));
    }

// 私有方法区域
private:
    // 静态转发方法，将 HTTP 请求转发给 xpay-go 内嵌 Gin engine
    // 参数 req：原始 HTTP 请求对象指针
    // 参数 targetPath：目标路径（xpay-go 中的路径）
    // 参数 cb：异步回调函数，用于返回转发后的响应
    static void forward(const drogon::HttpRequestPtr &req,
                        const std::string &targetPath,
                        std::function<void(const drogon::HttpResponsePtr &)> &&cb)
    {
        // 获取 Xpay 桥接单例实例
        auto &bridge = XpayBridge::instance();
        // 检查 xpay-go 桥接是否已初始化
        if (!bridge.ready()) {
            // 创建新的 HTTP 响应对象
            auto resp = drogon::HttpResponse::newHttpResponse();
            // 设置响应状态码为 503 Service Unavailable
            resp->setStatusCode(drogon::k503ServiceUnavailable);
            // 设置响应体，包含初始化失败的错误信息
            resp->setBody("xpay-go bridge not initialized: " + bridge.lastError());
            // 调用回调函数返回错误响应
            cb(resp);
            // 函数返回
            return;
        }

        // 获取原始请求的 HTTP 方法（GET、POST 等）
        std::string method = req->methodString();
        // 获取原始请求的查询字符串（URL 中 ? 后面的部分）
        std::string query  = req->getQuery();
        // 获取原始请求的请求体（POST 数据等）
        std::string body(req->getBody());

        // 创建用于存储请求头的 map 容器
        std::map<std::string, std::string> headers;
        // 遍历原始请求的所有请求头
        for (auto &[k, v] : req->headers()) {
            // 将请求头名称转换为小写用于比较
            std::string lk = k;
            // 使用 std::transform 将字符串中的所有字符转换为小写
            std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
            // 跳过 hop-by-hop 头（不应该转发的头）和 Content-Length 头
            if (lk == "content-length" || lk == "host" ||
                lk == "connection" || lk == "transfer-encoding") continue;
            // 将请求头添加到 headers map 中
            headers[k] = v;
        }
        // 添加 X-Forwarded-For 头，记录客户端真实 IP 给 xpay-go 用于防刷
        headers["X-Forwarded-For"] = req->getPeerAddr().toIp();

        // 调用 xpay-go 桥接的 request() 方法转发请求，获取响应结果
        auto r = bridge.request(method, targetPath, query, body, headers);

        // 创建新的 HTTP 响应对象用于返回给客户端
        auto resp = drogon::HttpResponse::newHttpResponse();
        // 设置响应状态码：如果 xpay-go 返回有效状态码则使用，否则返回 502 Bad Gateway
        resp->setStatusCode((drogon::HttpStatusCode)(r.status > 0 ? r.status : 502));
        // 遍历 xpay-go 返回的响应头
        for (auto &[k, v] : r.headers) {
            // 将响应头名称转换为小写用于比较
            std::string lk = k;
            // 使用 std::transform 将字符串中的所有字符转换为小写
            std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
            // 跳过 Content-Length 和 Transfer-Encoding 头（由 Drogon 自动处理）
            if (lk == "content-length" || lk == "transfer-encoding") continue;
            // 将响应头添加到响应对象中
            resp->addHeader(k, v);
        }
        // 设置响应体为 xpay-go 返回的响应内容（使用 move 避免复制）
        resp->setBody(std::move(r.body));
        // 调用回调函数返回响应给客户端
        cb(resp);
    }
// 类定义结束
};
