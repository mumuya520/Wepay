// WePay-Cpp — 支付通道插件基类
// 所有通道插件必须继承此基类并实现 createOrder / queryOrder / verifyNotify
#pragma once
#include <string>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <vector>
#include <functional>
#include <json/json.h>

struct ChannelOrderRequest {
    std::string orderId;          // 系统订单号
    std::string mchOrderNo;       // 商户订单号
    double      amount = 0;       // 金额(元)
    std::string subject;          // 商品名称
    std::string body;             // 商品描述
    std::string notifyUrl;        // 异步通知地址(系统的 /notify/channel/{code})
    std::string returnUrl;        // 同步跳转地址
    std::string clientIp;         // 客户端IP
    std::string payType;          // 支付类型
    Json::Value channelParams;    // 通道配置参数(从 pay_channel.params_json 解析)
};

struct ChannelOrderResult {
    bool   success = false;
    std::string errMsg;
    std::string payUrl;           // 支付链接(跳转/扫码)
    std::string qrCode;           // 二维码内容
    std::string channelOrderNo;   // 上游订单号
    std::string channelTradeNo;   // 上游交易号
    std::string rawResponse;      // 原始响应
    Json::Value extra;           // 额外数据
};

struct ChannelQueryResult {
    bool   success = false;
    std::string errMsg;
    int    tradeState = 0;        // 0=未支付 1=已支付 -1=已关闭
    std::string channelOrderNo;
    std::string channelTradeNo;
    std::string buyerId;
    double paidAmount = 0;
    std::string rawResponse;
};

struct ChannelNotifyResult {
    bool   verified = false;      // 验签是否通过
    bool   paid = false;          // 是否支付成功
    std::string orderId;          // 系统订单号
    std::string channelOrderNo;   // 上游订单号
    std::string buyerId;
    double paidAmount = 0;
    std::string errMsg;
    std::string responseText;     // 返回给上游的文本(如 "success")
};

// ── 退款 ────────────────────────────────────────────────
struct ChannelRefundRequest {
    std::string refundNo;         // 系统退款单号
    std::string channelOrderNo;   // 上游支付单号(必填，微信用 transaction_id)
    std::string orderId;          // 系统订单号
    double      paidAmount = 0;   // 原订单总金额
    double      refundAmount = 0; // 本次退款金额
    std::string reason;
    std::string notifyUrl;        // 退款回调URL
    std::string clientIp;         // 客户端IP
    Json::Value channelParams;
};
struct ChannelRefundResult {
    bool success = false;
    std::string errMsg;
    int  state = 0;               // 0=处理中 1=成功 -1=失败
    std::string channelRefundNo;  // 上游退款号
    double refundAmount = 0;      // 退款金额
    std::string rawResponse;
};

// ── 关闭订单 ────────────────────────────────────────────
struct ChannelCloseRequest {
    std::string orderId;
    std::string channelOrderNo;
    std::string clientIp;
    Json::Value channelParams;
};
struct ChannelCloseResult {
    bool success = false;
    std::string errMsg;
    std::string rawResponse;
};

// ── 转账 ────────────────────────────────────────────────
struct ChannelTransferRequest {
    std::string transferNo;
    double      amount = 0;
    int         accountType = 1;  // 1=微信openid 2=支付宝账号 3=银行卡
    std::string accountNo;
    std::string accountName;
    std::string remark;
    std::string notifyUrl;
    Json::Value channelParams;
};
struct ChannelTransferResult {
    bool success = false;
    std::string errMsg;
    int  state = 0;                // 0=处理中 1=成功 -1=失败
    std::string channelTransferNo; // 上游单号
};

// ── 分账执行 ────────────────────────────────────────────
struct DivisionReceiverItem {
    std::string accountNo;
    std::string accountName;
    int    accountType = 1;
    double amount = 0;             // 该接收方分账金额(元)
};
struct ChannelDivisionRequest {
    std::string orderId;
    std::string channelOrderNo;
    std::vector<DivisionReceiverItem> receivers;
    Json::Value channelParams;
};
struct ChannelDivisionResult {
    bool success = false;
    std::string errMsg;
    std::string channelDivisionNo;
};

// ── 获取 ChannelUserId(openid/buyer_id) ─────────────────
struct ChannelUserIdRequest {
    std::string code;              // OAuth2 授权码
    Json::Value channelParams;
};
struct ChannelUserIdResult {
    bool success = false;
    std::string errMsg;
    std::string userId;            // openid / buyer_id
    std::string nickname;
    std::string avatar;
};

// 通道插件基类
class ChannelPlugin {
public:
    virtual ~ChannelPlugin() = default;

    // 创建订单(调用上游接口)
    virtual ChannelOrderResult createOrder(const ChannelOrderRequest &req) = 0;

    // 查询订单(调用上游接口)
    virtual ChannelQueryResult queryOrder(const std::string &orderId,
                                          const Json::Value &channelParams) {
        ChannelQueryResult r;
        r.success = false;
        r.tradeState = 0;
        return r;
    }

    // 验证异步通知签名
    virtual ChannelNotifyResult verifyNotify(
        const std::map<std::string, std::string> &params,
        const std::string &rawBody,
        const Json::Value &channelParams) = 0;

    // 退款
    virtual ChannelRefundResult refund(const ChannelRefundRequest &req) {
        ChannelRefundResult r; r.errMsg = "该通道未实现退款"; return r;
    }
    // 验证退款异步通知
    virtual ChannelNotifyResult verifyRefundNotify(
        const std::map<std::string, std::string> &params,
        const std::string &rawBody,
        const Json::Value &channelParams) {
        return verifyNotify(params, rawBody, channelParams);  // 默认同支付通知
    }
    // 关闭订单
    virtual ChannelCloseResult close(const ChannelCloseRequest &req) {
        ChannelCloseResult r; r.success = true; return r;  // 默认本地关闭即可
    }
    // 转账
    virtual ChannelTransferResult transfer(const ChannelTransferRequest &req) {
        ChannelTransferResult r; r.errMsg = "该通道未实现转账"; return r;
    }
    // 分账执行
    virtual ChannelDivisionResult divisionExec(const ChannelDivisionRequest &req) {
        ChannelDivisionResult r; r.errMsg = "该通道未实现分账"; return r;
    }
    // 查询退款状态（调用上游接口查询退款单状态）
    virtual ChannelRefundResult queryRefund(const std::string &refundNo,
                                           const std::string &channelRefundNo,
                                           const Json::Value &channelParams) {
        ChannelRefundResult r;
        r.success = false;
        r.errMsg = "该通道不支持退款查询";
        return r;
    }

    // OAuth2 获取用户标识(openid/buyer_id)
    virtual ChannelUserIdResult queryChannelUserId(const ChannelUserIdRequest &req) {
        ChannelUserIdResult r; r.errMsg = "该通道不支持获取用户ID"; return r;
    }

    // 插件名称
    virtual std::string name() const = 0;

    // 通道参数 schema (用于通道管理页前端动态渲染表单)
    // 返回数组, 每项: {key, label, type:input|password|select|switch|number|textarea,
    //                  default, placeholder, required, options:[{value,label}], help}
    virtual Json::Value paramSchema() const {
        return Json::Value(Json::arrayValue);
    }
};

// 通道插件注册中心 + 安装态管理
class ChannelPluginRegistry {
public:
    using Creator = std::function<std::shared_ptr<ChannelPlugin>()>;

    static ChannelPluginRegistry &instance() {
        static ChannelPluginRegistry reg;
        return reg;
    }

    // 注册内置插件(编译时 REGISTER_CHANNEL_PLUGIN 宏自动调用)
    void registerPlugin(const std::string &name, Creator creator) {
        std::lock_guard<std::mutex> lock(mutex_);
        creators_[name] = std::move(creator);
    }

    // 标记某插件为"已安装"(用户在插件市场点安装后调用)
    void markInstalled(const std::string &name, bool installed = true) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (installed) installed_.insert(name);
        else installed_.erase(name);
    }

    // 查询是否已安装
    bool isInstalled(const std::string &name) {
        std::lock_guard<std::mutex> lock(mutex_);
        return installed_.count(name) > 0;
    }

    // 仅对已安装的插件返回实例
    std::shared_ptr<ChannelPlugin> create(const std::string &name) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!installed_.count(name)) return nullptr;
        auto it = creators_.find(name);
        if (it == creators_.end()) return nullptr;
        return it->second();
    }

    // 绕过安装检查直接创建(系统内部调用，如通道配置预览)
    std::shared_ptr<ChannelPlugin> createRaw(const std::string &name) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = creators_.find(name);
        if (it == creators_.end()) return nullptr;
        return it->second();
    }

    // 列出所有已编译可选插件(含未安装)
    std::vector<std::string> listPlugins() const {
        std::vector<std::string> names;
        for (auto &[k, _] : creators_) names.push_back(k);
        return names;
    }

    // 列出已安装
    std::vector<std::string> listInstalled() const {
        return std::vector<std::string>(installed_.begin(), installed_.end());
    }

    // 是否已编译(有实现体)
    bool hasBuiltIn(const std::string &name) const {
        return creators_.count(name) > 0;
    }

    // 从数据库同步已安装状态(程序启动时调用)
    void syncFromDb();

private:
    mutable std::mutex mutex_;
    std::map<std::string, Creator> creators_;
    std::set<std::string> installed_;
};

// 注册宏
#define REGISTER_CHANNEL_PLUGIN(cls) \
    static bool _reg_##cls = []{ \
        ChannelPluginRegistry::instance().registerPlugin( \
            cls().name(), []{ return std::make_shared<cls>(); }); \
        return true; \
    }()
