// WePay-Cpp — Gopay 支付服务
#pragma once // 防止头文件重复包含

#ifdef WEPAY_HAS_GOPAY // 可选的 Gopay 支持

#include <string> // 字符串库
#include <memory> // 智能指针
#include <thread> // 线程库
#include <mutex> // 互斥锁
#include <drogon/utils/Utilities.h>
#include "../gopay_client.hpp"

// GoPay 支付服务管理器
// 提供 GoPay 客户端的单例管理和订单操作接口
class GopayService {
// 私有成员
private:
    // GoPay 客户端单例实例（智能指针管理）
    static std::unique_ptr<gopay::Client> instance_;
    // 互斥锁，保护静态成员的线程安全访问
    static std::mutex mutex_;
    // 服务是否已启动标志
    static bool started_;
    // 初始化是否失败标志（用于避免重复尝试启动失败的服务）
    static bool init_failed_;

// 公共接口
public:
    // 获取 GoPay 客户端单例
    // 线程安全，首次调用时创建实例
    // 返回：GoPay 客户端引用
    static gopay::Client& getInstance() {
        // 获取互斥锁，保证线程安全
        std::lock_guard<std::mutex> lock(mutex_);
        // 如果实例不存在，创建新实例
        if (!instance_) {
            instance_ = std::make_unique<gopay::Client>();
        }
        // 返回实例引用
        return *instance_;
    }

    // 启动 GoPay 服务
    // 连接到 GoPay 服务器，初始化客户端
    // 参数 host：GoPay 服务器主机地址（默认 127.0.0.1）
    // 参数 port：GoPay 服务器端口（默认 9090）
    // 返回：true 表示启动成功，false 表示启动失败
    static bool start(const std::string& host = "127.0.0.1", const std::string& port = "9090") {
        // 获取互斥锁，保证线程安全
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 如果之前初始化失败，直接返回 false，避免重复尝试
        if (init_failed_) {
            LOG_WARN << "GoPay service initialization previously failed, skipping";
            return false;
        }

        // 如果服务已经启动，直接返回 true
        if (started_) {
            LOG_INFO << "GoPay service already started";
            return true;
        }

        // 如果实例不存在，创建新实例
        if (!instance_) {
            instance_ = std::make_unique<gopay::Client>();
        }

        // 尝试启动 GoPay 服务
        try {
            // 调用客户端的 start 方法启动服务
            bool ok = instance_->start("", host, port);
            // 检查启动是否成功
            if (ok) {
                // 标记服务已启动
                started_ = true;
                // 记录启动成功的日志
                LOG_INFO << "GoPay service started at " << host << ":" << port;
                // 记录 GoPay 版本信息
                LOG_INFO << "GoPay version: " << instance_->version();
                // 记录 GoPay 服务地址
                LOG_INFO << "GoPay address: " << instance_->addr();
                // 返回成功
                return true;
            } else {
                // 启动失败，记录错误日志
                LOG_ERROR << "Failed to start GoPay service (returned false)";
                // 标记初始化失败，避免后续重复尝试
                init_failed_ = true;
                // 返回失败
                return false;
            }
        } catch (const std::exception& e) {
            // 捕获标准异常，记录错误信息
            LOG_ERROR << "GoPay service start exception: " << e.what();
            // 标记初始化失败
            init_failed_ = true;
            // 返回失败
            return false;
        } catch (...) {
            // 捕获所有其他异常
            LOG_ERROR << "GoPay service start unknown exception";
            // 标记初始化失败
            init_failed_ = true;
            // 返回失败
            return false;
        }
    }

    // 停止 GoPay 服务
    // 断开与 GoPay 服务器的连接
    // 返回：true 表示停止成功，false 表示停止失败
    static bool stop() {
        // 获取互斥锁，保证线程安全
        std::lock_guard<std::mutex> lock(mutex_);
        
        // 如果服务未启动或实例不存在，直接返回 true
        if (!started_ || !instance_) {
            return true;
        }

        // 调用客户端的 stop 方法停止服务
        bool ok = instance_->stop();
        // 检查停止是否成功
        if (ok) {
            // 标记服务已停止
            started_ = false;
            // 记录停止成功的日志
            LOG_INFO << "GoPay service stopped";
        } else {
            // 记录停止失败的日志
            LOG_ERROR << "Failed to stop GoPay service";
        }
        
        // 返回停止结果
        return ok;
    }

    // 检查 GoPay 服务是否运行
    // 返回：true 表示服务正在运行，false 表示服务未运行
    static bool isRunning() {
        // 获取互斥锁，保证线程安全
        std::lock_guard<std::mutex> lock(mutex_);
        // 返回服务启动状态
        return started_;
    }

    // 创建订单
    // 参数 uid：用户 ID
    // 参数 out_trade_no：商户订单号
    // 参数 name：订单名称
    // 参数 notify_url：回调通知 URL
    // 参数 money：订单金额
    // 参数 pay_type：支付类型
    // 参数 channel_id：通道 ID
    // 参数 ip：用户 IP 地址
    // 返回：GoPay 响应对象
    static gopay::Response createOrder(
        uint32_t uid,
        const std::string& out_trade_no,
        const std::string& name,
        const std::string& notify_url,
        double money,
        int pay_type,
        int channel_id,
        const std::string& ip
    ) {
        // 调用客户端的 order_create 方法创建订单
        return getInstance().order_create(uid, out_trade_no, name, notify_url, money, pay_type, channel_id, ip);
    }

    // 查询订单
    // 参数 trade_no：GoPay 交易号
    // 返回：GoPay 响应对象（包含订单信息）
    static gopay::Response getOrder(const std::string& trade_no) {
        // 调用客户端的 order_get 方法查询订单
        return getInstance().order_get(trade_no);
    }

    // 获取订单列表
    // 参数 uid：用户 ID
    // 参数 page：页码（默认 1）
    // 参数 page_size：每页记录数（默认 20）
    // 返回：GoPay 响应对象（包含订单列表）
    static gopay::Response listOrders(uint32_t uid, int page = 1, int page_size = 20) {
        // 调用客户端的 order_list 方法获取订单列表
        return getInstance().order_list(uid, page, page_size);
    }

    // 标记订单已支付
    // 参数 trade_no：GoPay 交易号
    // 参数 api_trade_no：第三方支付平台的交易号
    // 返回：GoPay 响应对象
    static gopay::Response markPaid(const std::string& trade_no, const std::string& api_trade_no) {
        // 调用客户端的 order_paid 方法标记订单已支付
        return getInstance().order_paid(trade_no, api_trade_no);
    }

    // 申请退款
    // 参数 trade_no：GoPay 交易号
    // 参数 money：退款金额
    // 返回：GoPay 响应对象
    static gopay::Response refund(const std::string& trade_no, double money) {
        // 调用客户端的 order_refund 方法申请退款
        return getInstance().order_refund(trade_no, money);
    }
// 类定义结束
};

// ── 静态成员变量初始化 ──────────────────────────────
// GoPay 客户端单例实例初始化为 nullptr
std::unique_ptr<gopay::Client> GopayService::instance_;
// 互斥锁初始化
std::mutex GopayService::mutex_;
// 服务启动状态初始化为 false
bool GopayService::started_ = false;
// 初始化失败标志初始化为 false
bool GopayService::init_failed_ = false;

#else

// 如果没有启用 GoPay，提供空实现
class GopayService {
public:
    static bool start(const std::string& host = "127.0.0.1", const std::string& port = "9090") {
        LOG_WARN << "GoPay support not compiled in (WEPAY_HAS_GOPAY not defined)";
        return false;
    }
    
    static bool stop() { return true; }
    static bool isRunning() { return false; }
};

#endif
