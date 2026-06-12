// WePay-Cpp — WebSocket 订单状态推送控制器
//
// 用法:
//   前端: const ws = new WebSocket('ws://host/ws/order/' + orderId)
//         ws.onmessage = e => { const d = JSON.parse(e.data); if (d.state == 1) success(); }
//
//   后端: WsBus::instance().publish("order:" + orderId, {...})
#pragma once
#include <drogon/WebSocketController.h>
#include "../common/WsBus.h"
#include "../common/PayDb.h"

class WsOrderCtrl : public drogon::WebSocketController<WsOrderCtrl> {
public:
    void handleNewMessage(const drogon::WebSocketConnectionPtr &conn,
                          std::string &&message,
                          const drogon::WebSocketMessageType &) override {
        // 收到客户端心跳
        if (message == "ping") conn->send("pong");
    }

    void handleNewConnection(const drogon::HttpRequestPtr &req,
                             const drogon::WebSocketConnectionPtr &conn) override {
        // 从路径解析订单号: /ws/order/{orderId}
        std::string path = req->getPath();
        std::string orderId;
        const std::string prefix = "/ws/order/";
        if (path.size() > prefix.size() && path.substr(0, prefix.size()) == prefix) {
            orderId = path.substr(prefix.size());
        }
        if (orderId.empty()) {
            conn->send(R"({"code":-1,"msg":"missing order_id"})");
            conn->forceClose();
            return;
        }

        // 验证订单存在
        auto row = PayDb::instance().queryOne(
            "SELECT state,amount FROM pay_order WHERE order_id=?", {orderId});
        if (row.empty()) {
            conn->send(R"({"code":-1,"msg":"order not found"})");
            conn->forceClose();
            return;
        }

        std::string topic = "order:" + orderId;
        WsBus::instance().subscribe(topic, conn);
        conn->setContext(std::make_shared<std::string>(topic));

        // 推送当前状态
        Json::Value initial;
        initial["order_id"] = orderId;
        initial["state"] = std::stoi(row["state"]);
        initial["amount"] = row["amount"];
        initial["msg"] = "subscribed";
        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        conn->send(Json::writeString(wb, initial));
    }

    void handleConnectionClosed(const drogon::WebSocketConnectionPtr &conn) override {
        if (conn->hasContext()) {
            auto topic = conn->getContext<std::string>();
            if (topic) WsBus::instance().unsubscribe(*topic, conn);
        }
        WsBus::instance().unsubscribeAll(conn);
    }

    WS_PATH_LIST_BEGIN
        WS_PATH_ADD("/ws/order/{order_id}");
    WS_PATH_LIST_END
};
