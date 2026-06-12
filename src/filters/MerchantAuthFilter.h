// WePay-Cpp — 商户 JWT 鉴权中间件
// 保护 /merchant/api/** 路由 + 强制设备密钥绑定
#pragma once // 防止头文件重复包含
#include <drogon/HttpMiddleware.h> // Drogon HTTP 中间件
#include "../common/SimpleJwt.h" // JWT 令牌处理
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/DeviceKeyEnforcement.h" // 设备密钥强制检查

// 商户认证过滤器类
class MerchantAuthFilter : public drogon::HttpMiddleware<MerchantAuthFilter> {
public:
    static constexpr bool isAutoCreation = false; // 禁用自动创建

    // 中间件调用方法
    void invoke(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                drogon::MiddlewareNextCallback &&nextCb, // 下一个中间件回调
                drogon::MiddlewareCallback &&mcb) override { // 中间件完成回调
        std::string auth = req->getHeader("Authorization"); // 获取 Authorization 头
        if (auth.empty()) auth = req->getHeader("authorization"); // 尝试小写版本

        if (auth.empty()) { reject(mcb, "未授权，请先登录"); return; } // 认证头为空

        try {
            std::string token = SimpleJwt::fromHeader(auth); // 从头部提取 token
            std::string sub   = SimpleJwt::verify(token); // 验证 token 并获取 sub
            // sub 格式: "mch:<mch_id>:<username>"
            if (sub.substr(0, 4) != "mch:") { // 检查 token 类型
                throw std::runtime_error("not merchant token"); // 非商户 token
            }
            auto colonPos = sub.find(':', 4); // 查找第二个冒号
            std::string mchId    = sub.substr(4, colonPos - 4); // 提取商户 ID
            std::string username = (colonPos != std::string::npos) ? sub.substr(colonPos + 1) : ""; // 提取用户名

            req->addHeader("X-Mch-Id", mchId); // 添加商户 ID 头
            req->addHeader("X-Mch-User", username); // 添加用户名头
            req->addHeader("X-Mch-Username", username);  // 兼容旧代码

            // 强制设备密钥绑定检查
            int mchIdInt = 0; // 商户 ID 整数
            try { mchIdInt = std::stoi(mchId); } catch (...) {} // 转换为整数

            std::string path = req->getPath(); // 获取请求路径
            if (DeviceKeyEnforcement::requiresDeviceKeySetup(2, mchIdInt, path)) { // 检查是否需要设备密钥
                rejectDeviceKeyRequired(mcb); // 拒绝请求，要求绑定设备密钥
                return;
            }

            nextCb(std::move(mcb)); // 继续下一个中间件
        } catch (...) { // 捕获所有异常
            reject(mcb, "Token 无效或已过期，请重新登录"); // 返回错误响应
        }
    }

private:
    // 拒绝请求方法
    static void reject(drogon::MiddlewareCallback &mcb, const char *msg) { // 中间件回调和错误消息
        auto resp = drogon::HttpResponse::newHttpJsonResponse( // 创建 JSON 响应
            AjaxResult::error(401, msg)); // 401 错误响应
        resp->setStatusCode(drogon::k401Unauthorized); // 设置状态码为 401
        mcb(resp); // 返回响应
    }

    // 拒绝请求（需要设备密钥）方法
    static void rejectDeviceKeyRequired(drogon::MiddlewareCallback &mcb) { // 中间件回调
        Json::Value data; // 响应数据
        data["code"] = 403; // 错误代码
        data["msg"] = "您还未绑定设备密钥，请先完成设备绑定才能使用系统"; // 错误消息
        data["data"]["require_device_key_setup"] = true; // 需要设备密钥标志
        data["data"]["setup_url"] = "/merchant/api/auth/register-device"; // 设备注册 URL

        auto resp = drogon::HttpResponse::newHttpJsonResponse(data); // 创建 JSON 响应
        resp->setStatusCode(drogon::k403Forbidden); // 设置状态码为 403
        mcb(resp); // 返回响应
    }
};
