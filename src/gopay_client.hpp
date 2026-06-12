#pragma once // 防止头文件重复包含

#include <string> // 标准字符串库
#include <nlohmann/json.hpp> // JSON 库

// GoPay C API（Go 语言导出的 C 接口）
extern "C" {
    char* GoPay_Init(char* dbPath, char* port); // 初始化 GoPay 服务
    int GoPay_Start(char* dbPath, char* host, char* port); // 启动 GoPay 服务
    int GoPay_Stop(void); // 停止 GoPay 服务
    char* GoPay_Version(void); // 获取版本号
    char* GoPay_GetAddr(void); // 获取服务地址
    void GoPay_Free(char* ptr); // 释放 C 字符串内存
    char* GoPay_Call(char* method, char* params); // 调用 GoPay 方法
}

namespace gopay { // GoPay 命名空间

using json = nlohmann::json; // JSON 类型别名

// Go 字符串包装类（自动内存管理）
class GoString {
public:
    explicit GoString(char* ptr) : ptr_(ptr) {} // 构造函数，接收 C 字符串指针
    ~GoString() { if (ptr_) GoPay_Free(ptr_); } // 析构函数，自动释放内存
    GoString(const GoString&) = delete; // 禁用拷贝构造
    GoString& operator=(const GoString&) = delete; // 禁用赋值操作
    const char* c_str() const { return ptr_ ? ptr_ : ""; } // 获取 C 字符串
    std::string str() const { return ptr_ ? std::string(ptr_) : ""; } // 转换为 std::string
    json to_json() const { // 转换为 JSON 对象
        if (!ptr_) return json::object(); // 如果指针为空返回空对象
        try { return json::parse(ptr_); } // 尝试解析 JSON
        catch (...) { return json::object(); } // 解析失败返回空对象
    }
private:
    char* ptr_; // Go 字符串指针
};

// API 响应结构体
struct Response {
    int code; // 响应状态码
    std::string msg; // 响应消息
    json data; // 响应数据
    bool ok() const { return code == 0; } // 检查是否成功（状态码为 0）
    static Response from_json(const json& j) { // 从 JSON 构造响应对象
        Response r;
        r.code = j.value("code", -1); // 获取状态码，默认 -1
        r.msg = j.value("msg", ""); // 获取消息，默认空字符串
        r.data = j.value("data", json::object()); // 获取数据，默认空对象
        return r;
    }
};

// GoPay 客户端类
class Client {
public:
    // 初始化 GoPay 服务
    Response init(const std::string& db = "", const std::string& port = "8080") {
        auto d = const_cast<char*>(db.c_str()); // 转换为 C 字符串
        auto p = const_cast<char*>(port.c_str()); // 转换为 C 字符串
        return Response::from_json(GoString(GoPay_Init(d, p)).to_json()); // 调用 C API 并解析响应
    }

    // 启动 GoPay 服务
    bool start(const std::string& db = "", const std::string& host = "0.0.0.0", const std::string& port = "8080") {
        auto d = const_cast<char*>(db.c_str()); // 转换为 C 字符串
        auto h = const_cast<char*>(host.c_str()); // 转换为 C 字符串
        auto p = const_cast<char*>(port.c_str()); // 转换为 C 字符串
        return GoPay_Start(d, h, p) == 0; // 调用 C API，返回成功状态
    }

    // 停止 GoPay 服务
    bool stop() { return GoPay_Stop() == 0; }
    // 获取版本号
    std::string version() { return GoString(GoPay_Version()).str(); }
    // 获取服务地址
    std::string addr() { return GoString(GoPay_GetAddr()).str(); }

    // 通用方法调用接口
    Response call(const std::string& method, const json& params = json::object()) {
        std::string ps = params.dump(); // 将 JSON 转换为字符串
        auto m = const_cast<char*>(method.c_str()); // 转换为 C 字符串
        auto p = const_cast<char*>(ps.c_str()); // 转换为 C 字符串
        return Response::from_json(GoString(GoPay_Call(m, p)).to_json()); // 调用 C API 并解析响应
    }

    // ═══════════════════════════════════════════════════════════════
    // 订单相关方法
    // ═══════════════════════════════════════════════════════════════
    // 创建订单
    Response order_create(uint32_t uid, const std::string& out_trade_no, const std::string& name,
                         const std::string& notify_url, double money, int pay_type, int channel_id, const std::string& ip) {
        return call("order.create", { // 调用 order.create 方法
            {"uid", uid}, // 用户 ID
            {"out_trade_no", out_trade_no}, // 外部订单号
            {"name", name}, // 商品名称
            {"notify_url", notify_url}, // 异步通知 URL
            {"return_url", ""}, // 同步返回 URL
            {"param", ""}, // 额外参数
            {"money", money}, // 金额
            {"pay_type", pay_type}, // 支付类型
            {"channel_id", channel_id}, // 通道 ID
            {"ip", ip} // 客户端 IP
        });
    }

    // 查询订单
    Response order_get(const std::string& trade_no) {
        return call("order.get", {{"trade_no", trade_no}}); // 按订单号查询
    }

    // 获取订单列表
    Response order_list(uint32_t uid, int page = 1, int page_size = 20) {
        return call("order.list", { // 获取用户订单列表
            {"uid", uid}, // 用户 ID
            {"status", -1}, // 订单状态（-1 表示所有）
            {"page", page}, // 页码
            {"page_size", page_size} // 每页数量
        });
    }

    // 标记订单已支付
    Response order_paid(const std::string& trade_no, const std::string& api_trade_no) {
        return call("order.paid", { // 标记订单为已支付
            {"trade_no", trade_no}, // 内部订单号
            {"api_trade_no", api_trade_no}, // 第三方订单号
            {"buyer", ""} // 买家信息
        });
    }

    // 申请退款
    Response order_refund(const std::string& trade_no, double money) {
        return call("order.refund", { // 申请订单退款
            {"trade_no", trade_no}, // 订单号
            {"money", money} // 退款金额
        });
    }

    // ═══════════════════════════════════════════════════════════════
    // 用户相关方法
    // ═══════════════════════════════════════════════════════════════
    // 用户登录
    Response user_login(uint32_t uid, const std::string& password) {
        return call("user.login", { // 用户登录
            {"uid", uid}, // 用户 ID
            {"password", password} // 密码
        });
    }

    // 用户注册
    Response user_register(const std::string& email, const std::string& phone, const std::string& password) {
        return call("user.register", { // 注册新用户
            {"email", email}, // 邮箱
            {"phone", phone}, // 手机号
            {"password", password}, // 密码
            {"invite_code", ""}, // 邀请码
            {"ip", ""} // IP 地址
        });
    }

    // 获取用户信息
    Response user_get(uint32_t uid) {
        return call("user.get", {{"uid", uid}}); // 获取用户详细信息
    }

    // ═══════════════════════════════════════════════════════════════
    // 余额相关方法
    // ═══════════════════════════════════════════════════════════════
    // 获取账户余额
    Response balance_get(uint32_t uid) {
        return call("transfer.balance", {{"uid", uid}}); // 查询用户余额
    }

    // 获取余额变动记录
    Response balance_records(uint32_t uid, int page = 1, int page_size = 20) {
        return call("transfer.records", { // 获取余额变动历史
            {"uid", uid}, // 用户 ID
            {"action", -1}, // 操作类型（-1 表示所有）
            {"page", page}, // 页码
            {"page_size", page_size} // 每页数量
        });
    }

    // ═══════════════════════════════════════════════════════════════
    // 结算相关方法
    // ═══════════════════════════════════════════════════════════════
    // 申请结算
    Response settle_apply(uint32_t uid, const std::string& account, const std::string& username, double money, int type) {
        return call("settle.apply", { // 申请提现
            {"uid", uid}, // 用户 ID
            {"account", account}, // 提现账户
            {"username", username}, // 账户名称
            {"money", money}, // 提现金额
            {"settle_type", type} // 提现类型
        });
    }

    // 获取结算列表
    Response settle_list(uint32_t uid, int page = 1, int page_size = 20) {
        return call("settle.list", { // 获取提现历史
            {"uid", uid}, // 用户 ID
            {"page", page}, // 页码
            {"page_size", page_size} // 每页数量
        });
    }

    // ═══════════════════════════════════════════════════════════════
    // 管理员相关方法
    // ═══════════════════════════════════════════════════════════════
    // 管理员登录
    Response admin_login(const std::string& username, const std::string& password) {
        return call("admin.login", { // 管理员登录
            {"username", username}, // 用户名
            {"password", password} // 密码
        });
    }

    // 获取管理员令牌
    Response admin_token() {
        return call("admin.token"); // 获取管理员 JWT 令牌
    }
};

} // namespace gopay
