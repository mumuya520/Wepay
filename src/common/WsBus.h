// WePay-Cpp — WebSocket 消息总线
// 维护 orderId → 已订阅的 WebSocket 连接，支付成功时主动推送给前端
//
// 使用流程:
//   前端: ws://host/ws/order/W123456789
//   后端: WsBus::publish(orderId, json) → 所有订阅该订单的连接收到
#pragma once // 防止头文件重复包含
#include <drogon/WebSocketConnection.h> // Drogon WebSocket 连接
#include <string> // 字符串库
#include <unordered_map> // 哈希表
#include <unordered_set> // 哈希集合
#include <mutex> // 互斥锁
#include <memory> // 智能指针
#include <json/json.h> // JSON 库

// WebSocket 消息总线类
class WsBus {
public:
    // WebSocket 连接指针类型别名
    using ConnPtr = drogon::WebSocketConnectionPtr;

    // 获取单例
    static WsBus &instance() { static WsBus b; return b; }

    // 添加订阅方法
    void subscribe(const std::string &topic, const ConnPtr &conn) { // topic 主题，conn 连接指针
        std::lock_guard<std::mutex> lock(mutex_); // 加锁保护
        topics_[topic].insert(conn); // 将连接添加到主题集合
    }

    // 移除订阅方法（连接关闭时）
    void unsubscribe(const std::string &topic, const ConnPtr &conn) { // topic 主题，conn 连接指针
        std::lock_guard<std::mutex> lock(mutex_); // 加锁保护
        auto it = topics_.find(topic); // 查找主题
        if (it == topics_.end()) return; // 主题不存在则返回
        it->second.erase(conn); // 从集合中删除连接
        if (it->second.empty()) topics_.erase(it); // 如果集合为空则删除主题
    }

    // 移除所有订阅方法（连接断开）
    void unsubscribeAll(const ConnPtr &conn) { // conn 连接指针
        std::lock_guard<std::mutex> lock(mutex_); // 加锁保护
        for (auto it = topics_.begin(); it != topics_.end(); ) { // 遍历所有主题
            it->second.erase(conn); // 从集合中删除连接
            if (it->second.empty()) it = topics_.erase(it); // 如果集合为空则删除主题
            else ++it; // 否则移动到下一个主题
        }
    }

    // 推送消息到指定 topic 的所有订阅者方法
    void publish(const std::string &topic, const Json::Value &message) { // topic 主题，message JSON 消息
        Json::StreamWriterBuilder wb; wb["indentation"] = ""; // 创建 JSON 写入器
        std::string body = Json::writeString(wb, message); // 将 JSON 转换为字符串

        std::vector<ConnPtr> targets; // 目标连接列表
        {
            std::lock_guard<std::mutex> lock(mutex_); // 加锁保护
            auto it = topics_.find(topic); // 查找主题
            if (it == topics_.end()) return; // 主题不存在则返回
            targets.assign(it->second.begin(), it->second.end()); // 复制所有连接
        }
        for (auto &c : targets) { // 遍历所有目标连接
            if (c->connected()) c->send(body); // 如果连接已建立则发送消息
        }
    }

    // 广播到所有 topic 前缀匹配的订阅者方法（例: "live:" 匹配 "live:abc", "live:def"）
    void broadcast(const std::string &prefix, const Json::Value &message) { // prefix 前缀，message JSON 消息
        Json::StreamWriterBuilder wb; wb["indentation"] = ""; // 创建 JSON 写入器
        std::string body = Json::writeString(wb, message); // 将 JSON 转换为字符串
        std::vector<ConnPtr> targets; // 目标连接列表
        {
            std::lock_guard<std::mutex> lock(mutex_); // 加锁保护
            for (auto &kv : topics_) { // 遍历所有主题
                if (kv.first.size() >= prefix.size() && // 如果主题长度足够
                    kv.first.substr(0, prefix.size()) == prefix) { // 且前缀匹配
                    targets.insert(targets.end(), kv.second.begin(), kv.second.end()); // 添加所有连接
                }
            }
        }
        for (auto &c : targets) { // 遍历所有目标连接
            if (c->connected()) c->send(body); // 如果连接已建立则发送消息
        }
    }

    // 快捷方法：广播实时事件到所有 live 连接
    void publishLive(const std::string &event, const Json::Value &data) { // event 事件名，data 数据
        Json::Value msg; // 创建消息对象
        msg["event"] = event; // 设置事件名
        msg["data"]  = data; // 设置数据
        msg["ts"]    = (Json::Int64)std::time(nullptr); // 设置时间戳
        broadcast("live:", msg); // 广播到所有 live 前缀的连接
    }

    // 获取指定主题的订阅者数量方法（调试用）
    size_t subscribers(const std::string &topic) { // topic 主题
        std::lock_guard<std::mutex> lock(mutex_); // 加锁保护
        auto it = topics_.find(topic); // 查找主题
        return it == topics_.end() ? 0 : it->second.size(); // 返回订阅者数量
    }

    // 获取总主题数量方法
    size_t totalTopics() {
        std::lock_guard<std::mutex> lock(mutex_); // 加锁保护
        return topics_.size(); // 返回主题总数
    }

private:
    std::mutex mutex_; // 互斥锁
    std::unordered_map<std::string, std::unordered_set<ConnPtr>> topics_; // 主题 → 连接集合映射
};
