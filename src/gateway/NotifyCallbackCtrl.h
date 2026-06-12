// WePay-Cpp — 异步通知接口（商户回调）
// POST /api/pay/notify        支付结果异步通知
#pragma once
#include <drogon/HttpController.h>
#include "../common/AjaxResult.h"
#include "../common/PayDb.h"
#include "../common/NotifyTaskService.h"
#include <json/json.h>
#include <ctime>

using namespace drogon;

class NotifyCallbackCtrl : public drogon::HttpController<NotifyCallbackCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(NotifyCallbackCtrl::payNotify, "/api/pay/notify", drogon::Post);
    METHOD_LIST_END

    // 支付结果异步通知（由支付通道回调，然后转发给商户）
    void payNotify(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto j = req->getJsonObject();
        if (!j) {
            // 尝试解析表单数据
            auto params = req->getParameters();
            if (params.empty()) {
                RESP_ERR(cb, "参数格式错误");
                return;
            }

            // 从表单参数构建 JSON
            Json::Value jsonData;
            for (auto &[k, v] : params) {
                jsonData[k] = v;
            }
            j = std::make_shared<Json::Value>(jsonData);
        }

        // 提取订单号
        std::string orderId = (*j).get("order_id", "").asString();
        if (orderId.empty()) {
            orderId = (*j).get("out_trade_no", "").asString();
        }

        if (orderId.empty()) {
            RESP_ERR(cb, "订单号不能为空");
            return;
        }

        auto &db = PayDb::instance();

        // 查询订单
        auto order = db.queryOne(
            "SELECT o.order_id,o.mch_id,o.mch_order_no,o.amount,o.real_amount,o.pay_type,"
            "o.state,o.pay_time,o.notify_url,m.mch_no "
            "FROM pay_order o "
            "LEFT JOIN merchant m ON m.id=o.mch_id "
            "WHERE o.order_id=? OR o.mch_order_no=?",
            {orderId, orderId});

        if (order.empty()) {
            RESP_ERR(cb, "订单不存在");
            return;
        }

        // 检查订单状态
        int state = std::stoi(order["state"]);
        if (state != 1) {
            // 订单未支付，更新为已支付
            long long now = std::time(nullptr);

            db.exec(
                "UPDATE pay_order SET state=1,pay_time=?,real_amount=?,updated_at=? WHERE order_id=?",
                {std::to_string(now), order["amount"], std::to_string(now), order["order_id"]});

            // 更新商户余额
            db.exec(
                "UPDATE merchant SET balance=balance+CAST(? AS NUMERIC),total_income=total_income+CAST(? AS NUMERIC) WHERE id=?",
                {order["amount"], order["amount"], order["mch_id"]});

            // 记录资金日志
            db.exec(
                "INSERT INTO money_log(mch_id,change_type,change_amount,before_amount,after_amount,biz_type,biz_no,remark,created_at) "
                "SELECT ?,1,?,balance,balance+CAST(? AS NUMERIC),'pay',?,'支付收入',? FROM merchant WHERE id=?",
                {order["mch_id"], order["amount"], order["amount"], order["order_id"],
                 std::to_string(now), order["mch_id"]});
        }

        // 触发异步通知任务（通知商户）
        std::string notifyUrl = order["notify_url"];
        if (!notifyUrl.empty()) {
            NotifyTaskService::createTaskAndSend(order["order_id"], notifyUrl, "NotifyCallbackCtrl");
        }

        // 返回成功
        auto resp = HttpResponse::newHttpResponse();
        resp->setContentTypeCode(CT_TEXT_PLAIN);
        resp->setBody("success");
        cb(resp);
    }
};
