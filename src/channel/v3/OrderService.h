#pragma once
#include <drogon/HttpController.h>
#include "SecurityValidator.h"
#include "DeviceManager.h"
#include "WebSocketController.h"
#include "EmailService.h"

namespace wepay {
namespace v3 {

// 订单信息
struct OrderInfo {
    std::string orderId;
    std::string merchantOrderId;
    std::string merchantId;
    std::string deviceId;
    std::string qrId;
    std::string qrCodeType = "PERSONAL"; // PERSONAL / BUSINESS
    std::string qrCodeName;
    std::string qrCodeContent;
    double amount = 0.0;
    std::string payType;        // "ALIPAY" / "WECHAT"
    std::string status;         // "PENDING" / "PUSHING" / "PAID" / "FAILED" / "TIMEOUT"
    std::string screenshotUrl;
    std::string notifyEmail;   // 可选，下单时指定收件邮箱；为空则用商户默认 notify_email
    int64_t createTime = 0;
    int64_t payTime = 0;
    int64_t expireTime = 0;
};

// 订单服务
class OrderService {
public:
    OrderService();

    // 创建订单
    bool createOrder(const OrderInfo& order);

    // 推送订单到设备
    bool pushOrderToDevice(const std::string& orderId);

    // 更新订单状态
    bool updateOrderStatus(const std::string& orderId, const std::string& status);

    // 查询订单
    std::optional<OrderInfo> getOrder(const std::string& orderId);

    // 查询待推送订单（设备轮询接口）
    std::vector<OrderInfo> getPendingOrders(const std::string& deviceId);

    // 订单支付确认
    bool confirmOrderPaid(const std::string& orderId, const std::string& screenshotUrl);

    // 订单超时处理
    void handleOrderTimeout(const std::string& orderId);

    // 幂等性检查
    bool checkOrderIdempotent(const std::string& orderId);

    // 通知栏金额匹配（按金额+支付类型反查待支付订单）
    std::optional<OrderInfo> matchOrderByAmount(const std::string& merchantId,
                                                double amount,
                                                const std::string& payType);

    // 触发商户回调（public，供 handleNotify 直接调用）
    bool triggerMerchantCallback(const OrderInfo& order);

    // 注入邮件服务
    void setEmailService(std::shared_ptr<EmailService> svc) { emailService_ = svc; }

    // 发送订单邮件通知（PAID/FAILED/TIMEOUT）
    // needCallbackBtn：为 true 时邮件包含手动回调按钮
    void sendOrderEmail(const std::string& orderId,
                        const std::string& statusType,
                        bool needCallbackBtn = true);

private:
    struct DeviceQrCode {
        std::string id;
        std::string deviceId;
        std::string merchantId;
        std::string payType;
        std::string codeType;
        std::string codeName;
        std::string codeContent;
    };

    struct QrSelectionPreference {
        std::string codeType;
        bool supportBusinessCode = true;
    };

    std::shared_ptr<DeviceManager> deviceManager_;
    std::shared_ptr<EmailService> emailService_;

    // 选择设备推送订单
    std::string selectDevice(const std::string& merchantId);

    // 获取商户/通道维度的收款码偏好
    QrSelectionPreference resolveQrPreference(const std::string& merchantNo,
                                              const std::string& payType);

    // 选择设备下可用收款码
    std::optional<DeviceQrCode> selectQrCode(const std::string& merchantId,
                                             const std::string& deviceId,
                                             const std::string& payType,
                                             const std::string& preferredCodeType = "");

    // 发送到RocketMQ
    bool sendToRocketMQ(const std::string& topic, const nlohmann::json& message);
};

// V3订单推送接口控制器
class OrderPushController : public drogon::HttpController<OrderPushController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(OrderPushController::handleNotify,
                  "/api/wepay/v3/notify", drogon::Post);
    ADD_METHOD_TO(OrderPushController::handleNotify,
                  "/device/notify",       drogon::Post);
    METHOD_LIST_END

    OrderPushController();

    // 处理支付推送（主动确认：设备知道orderId）
    void handlePush(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    // 处理待单查询（设备轮询兜底）
    void handlePending(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    // 通知栏监听上报（被动监听：收到金额，服务器反查匹配）
    void handleNotify(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback);

private:
    std::shared_ptr<SecurityValidator> validator_;
    std::shared_ptr<OrderService> orderService_;

    drogon::HttpResponsePtr buildResponse(int code, const std::string& message,
                                         const nlohmann::json& data = {});
};

} // namespace v3
} // namespace wepay
