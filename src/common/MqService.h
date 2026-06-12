// WePay-Cpp — 消息队列抽象层
// 默认使用本地线程队列(走 NotifyTaskService 的现有实现)
// 若编译时定义 WEPAY_HAS_RABBITMQ 且配置 mq.enabled=true，则走 RabbitMQ
// 提供简洁的 publish/consume 接口，供异步通知/日志/分账等场景解耦使用
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <functional> // 函数库
#include <mutex> // 互斥锁
#include <queue> // 队列容器
#include <thread> // 线程库
#include <atomic> // 原子操作
#include <iostream> // 输入输出库
#include <json/json.h> // JSON 库

#ifdef WEPAY_HAS_RABBITMQ // 可选的 RabbitMQ 支持
#  if __has_include(<rabbitmq-c/amqp.h>) // 检查是否有 rabbitmq-c 头文件
#    include <rabbitmq-c/amqp.h> // RabbitMQ C 库
#    include <rabbitmq-c/tcp_socket.h> // RabbitMQ TCP 套接字
#  else // 备选头文件路径
#    include <amqp.h> // AMQP 库
#    include <amqp_tcp_socket.h> // AMQP TCP 套接字
#  endif
#endif

// 消息队列服务类
class MqService {
public:
    // 消息处理器类型别名
    using Handler = std::function<void(const std::string &topic, const std::string &body)>;

    // 获取单例实例
    static MqService &instance() {
        static MqService m; // 静态单例
        return m;
    }

    // 配置消息队列方法
    void configure(const Json::Value &cfg) {
        enabled_ = cfg.get("enabled", false).asBool(); // 读取启用标志
        type_    = cfg.get("type", "local").asString(); // 读取队列类型
#ifdef WEPAY_HAS_RABBITMQ // 如果编译了 RabbitMQ 支持
        if (enabled_ && type_ == "rabbitmq") { // 如果启用且类型为 RabbitMQ
            std::string host = cfg.get("host", "127.0.0.1").asString(); // 读取主机地址
            int port         = cfg.get("port", 5672).asInt(); // 读取端口号
            std::string user = cfg.get("username", "guest").asString(); // 读取用户名
            std::string pwd  = cfg.get("password", "guest").asString(); // 读取密码
            std::string vhost= cfg.get("vhost", "/").asString(); // 读取虚拟主机
            conn_ = amqp_new_connection(); // 创建新连接
            amqp_socket_t *sock = amqp_tcp_socket_new(conn_); // 创建 TCP 套接字
            if (!sock || amqp_socket_open(sock, host.c_str(), port)) { // 如果套接字创建或打开失败
                std::cerr << "[MqService] RabbitMQ 连接失败，回退本地模式" << std::endl; // 输出错误信息
                amqp_destroy_connection(conn_); conn_ = nullptr; // 销毁连接
                type_ = "local"; // 回退到本地模式
            } else { // 连接成功
                auto rep = amqp_login(conn_, vhost.c_str(), 0, 131072, 0, // 执行登录
                    AMQP_SASL_METHOD_PLAIN, user.c_str(), pwd.c_str()); // 使用 PLAIN 认证
                if (rep.reply_type != AMQP_RESPONSE_NORMAL) { // 如果认证失败
                    std::cerr << "[MqService] RabbitMQ AUTH 失败，回退本地模式" << std::endl; // 输出错误信息
                    amqp_destroy_connection(conn_); conn_ = nullptr; // 销毁连接
                    type_ = "local"; // 回退到本地模式
                } else { // 认证成功
                    amqp_channel_open(conn_, 1); // 打开通道 1
                    std::cout << "[MqService] RabbitMQ 连接成功 " << host << ":" << port << std::endl; // 输出成功信息
                }
            }
        }
#else // 如果未编译 RabbitMQ 支持
        if (enabled_ && type_ == "rabbitmq") { // 如果启用且类型为 RabbitMQ
            std::cerr << "[MqService] 未编译 WEPAY_HAS_RABBITMQ，回退本地模式" << std::endl; // 输出警告信息
            type_ = "local"; // 回退到本地模式
        }
#endif
        std::cout << "[MqService] 模式=" << type_ << " enabled=" << enabled_ << std::endl; // 输出配置信息
    }

    // 发布消息到 topic(exchange="wepay",routing_key=topic) 方法
    void publish(const std::string &topic, const std::string &body) { // topic 主题，body 消息体
        if (!enabled_) { return; } // 如果未启用则直接返回
#ifdef WEPAY_HAS_RABBITMQ // 如果编译了 RabbitMQ 支持
        if (type_ == "rabbitmq" && conn_) { // 如果类型为 RabbitMQ 且连接存在
            amqp_bytes_t exch = amqp_cstring_bytes("wepay"); // 创建交换机名称
            amqp_bytes_t rk   = amqp_cstring_bytes(topic.c_str()); // 创建路由键
            amqp_bytes_t payload; // 创建负载
            payload.len = body.size(); // 设置负载长度
            payload.bytes = (void*)body.data(); // 设置负载数据
            amqp_basic_publish(conn_, 1, exch, rk, 0, 0, nullptr, payload); // 发布消息
            return; // 返回
        }
#endif
        // 本地模式: 入队 + 唤醒消费者线程
        std::lock_guard<std::mutex> lock(mutex_); // 获取互斥锁
        queue_.push({topic, body}); // 将消息入队
    }

    // 注册本地消费者方法
    void subscribe(const std::string &topic, Handler handler) { // topic 主题，handler 处理器
        std::lock_guard<std::mutex> lock(mutex_); // 获取互斥锁
        handlers_[topic] = std::move(handler); // 注册处理器
        if (!workerRunning_) { // 如果工作线程未运行
            workerRunning_ = true; // 设置工作线程运行标志
            std::thread([this] { workerLoop(); }).detach(); // 启动工作线程
        }
    }

    // 关闭消息队列方法
    void shutdown() {
        workerRunning_ = false; // 停止工作线程
#ifdef WEPAY_HAS_RABBITMQ // 如果编译了 RabbitMQ 支持
        if (conn_) { // 如果连接存在
            amqp_channel_close(conn_, 1, AMQP_REPLY_SUCCESS); // 关闭通道
            amqp_connection_close(conn_, AMQP_REPLY_SUCCESS); // 关闭连接
            amqp_destroy_connection(conn_); // 销毁连接
            conn_ = nullptr; // 清空连接指针
        }
#endif
    }

    // 获取队列类型方法
    const std::string &type() const { return type_; } // 返回队列类型
    // 获取启用状态方法
    bool enabled() const { return enabled_; } // 返回启用状态

// 私有区域
private:
    // ── 消息结构体 ──────────────────────────────
    // 消息数据结构
    struct Msg {
        // 消息主题（用于路由）
        std::string topic;
        // 消息体（JSON 或其他格式）
        std::string body;
    };

    // ── 工作线程循环 ──────────────────────────────
    // 消费者工作线程的主循环
    // 不断从队列中取出消息，调用对应的处理器
    void workerLoop() {
        // 当工作线程运行时
        while (workerRunning_) {
            // 消息变量
            Msg msg;
            // 是否有消息标志
            bool has = false;
            // 作用域：获取消息
            {
                // 获取互斥锁保护队列访问
                std::lock_guard<std::mutex> lock(mutex_);
                // 如果队列不为空
                if (!queue_.empty()) {
                    // 获取队列前面的消息
                    msg = std::move(queue_.front());
                    // 移除队列前面的消息
                    queue_.pop();
                    // 设置有消息标志
                    has = true;
                }
            } // 作用域结束，释放互斥锁
            // 如果有消息
            if (has) {
                // 处理器变量
                Handler h;
                // 作用域：获取处理器
                {
                    // 获取互斥锁保护处理器映射访问
                    std::lock_guard<std::mutex> lock(mutex_);
                    // 查找对应 topic 的处理器
                    auto it = handlers_.find(msg.topic);
                    // 如果找到则获取处理器
                    if (it != handlers_.end())
                        h = it->second;
                } // 作用域结束，释放互斥锁
                // 如果处理器存在
                if (h) {
                    // 调用处理器，传递 topic 和消息体
                    try {
                        h(msg.topic, msg.body);
                    } catch (const std::exception &e) {
                        // 捕获异常并输出错误信息
                        std::cerr << "[MqService] 消费异常: " << e.what() << std::endl;
                    }
                }
            } else {
                // 如果没有消息，睡眠 200 毫秒避免忙轮询
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
    }

    // ── 成员变量 ──────────────────────────────
    // 消息队列启用标志（默认为 false）
    bool enabled_ = false;
    // 队列类型（"local" 或 "rabbitmq"，默认为 "local"）
    std::string type_ = "local";
    // 互斥锁（保护队列和处理器映射的并发访问）
    std::mutex mutex_;
    // 消息队列（存储待处理的消息）
    std::queue<Msg> queue_;
    // 处理器映射（topic → Handler）
    std::unordered_map<std::string, Handler> handlers_;
    // 工作线程运行标志（原子操作）
    std::atomic<bool> workerRunning_{false};
// 如果编译了 RabbitMQ 支持
#ifdef WEPAY_HAS_RABBITMQ
    // RabbitMQ 连接指针（nullptr 表示未连接）
    amqp_connection_state_t conn_ = nullptr;
#endif
}; // 类定义结束
