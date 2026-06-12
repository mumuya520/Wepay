// WePay-Cpp — 企业微信客服支付接口 ( wework.php)
// GET  /wework.php  — URL 验证 (echostr)
// POST /wework.php  — 接收企业微信消息回调
// 支持:
//   1. 关注事件 → 发送欢迎菜单
//   2. 文本消息 → 识别金额并生成支付链接
//   3. 点击事件 → 菜单跳转
#pragma once
#include <drogon/HttpController.h>
#include <string>
#include <sstream>
#include <ctime>
#include "../common/PayDb.h"
#include "../common/Md5Utils.h"
#include "../common/ChannelService.h"

class WeWorkCtrl : public drogon::HttpController<WeWorkCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(WeWorkCtrl::verify,   "/wework.php", drogon::Get);
        ADD_METHOD_TO(WeWorkCtrl::callback, "/wework.php", drogon::Post);
    METHOD_LIST_END

    // GET — 企业微信 URL 验证
    void verify(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string echostr = req->getParameter("echostr");
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        resp->setBody(echostr);
        cb(resp);
    }

    // POST — 接收消息/事件
    void callback(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto &db = PayDb::instance();
        std::string weworkEnabled = db.getSetting("wework_enabled", "0");
        if (weworkEnabled != "1") {
            auto r = drogon::HttpResponse::newHttpResponse();
            r->setBody(""); cb(r); return;
        }

        std::string body = std::string(req->bodyData(), req->bodyLength());
        // 简单 XML 解析（企业微信回调为 XML 格式）
        std::string msgType = extractXml(body, "MsgType");
        std::string fromUser = extractXml(body, "FromUserName");
        std::string toUser = extractXml(body, "ToUserName");

        std::string reply;

        if (msgType == "event") {
            std::string event = extractXml(body, "Event");
            if (event == "subscribe" || event == "enter_agent") {
                // 关注/进入应用 → 发送欢迎消息
                std::string siteName = db.getSetting("siteName", "WePay");
                std::string welcome = db.getSetting("wework_welcome",
                    "欢迎使用" + siteName + "支付！\n"
                    "发送金额即可生成付款链接。\n"
                    "例如发送：9.9");
                reply = buildTextReply(fromUser, toUser, welcome);
            } else if (event == "click") {
                std::string eventKey = extractXml(body, "EventKey");
                if (eventKey == "PAY") {
                    reply = buildTextReply(fromUser, toUser,
                        "请直接发送金额（如 9.9），我将为您生成付款链接。");
                }
            }
        } else if (msgType == "text") {
            std::string content = extractXml(body, "Content");
            // 尝试解析为金额
            double amount = 0;
            try { amount = std::stod(content); } catch (...) {}

            if (amount > 0) {
                // 生成支付链接
                std::string siteUrl = db.getSetting("siteUrl", "");
                std::string mchIdStr = db.getSetting("wework_mch_id", "1");
                int mchId = 1;
                try { mchId = std::stoi(mchIdStr); } catch (...) {}

                std::string orderId = ChannelService::generateOrderId(
                    db.getSetting("order_prefix", "W"));
                long long now = std::time(nullptr);
                int closeMin = 5;
                try { closeMin = std::stoi(db.getSetting("close_minutes", "5")); } catch (...) {}

                // 创建订单
                db.exec(
                    "INSERT INTO pay_order(order_id,mch_id,mch_order_no,pay_type,"
                    "amount,real_amount,subject,state,expire_time,created_at,updated_at) "
                    "VALUES(?,?,?,?,?,?,?,0,?,?,?)",
                    {orderId, std::to_string(mchId), "wework_" + orderId, "wxpay",
                     ChannelService::fmtAmount(amount), ChannelService::fmtAmount(amount),
                     "企微客服支付",
                     std::to_string(now + closeMin * 60),
                     std::to_string(now), std::to_string(now)});

                std::string payUrl = siteUrl + "/gateway/cashier/" + orderId;
                reply = buildTextReply(fromUser, toUser,
                    "已为您生成支付链接：\n"
                    "金额：¥" + ChannelService::fmtAmount(amount) + "\n"
                    "订单号：" + orderId + "\n"
                    "请点击链接完成支付：\n" + payUrl);
            } else {
                reply = buildTextReply(fromUser, toUser,
                    "请发送正确的金额（如 9.9），我将为您生成付款链接。");
            }
        }

        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setContentTypeCode(drogon::CT_TEXT_XML);
        resp->setBody(reply.empty() ? "" : reply);
        cb(resp);
    }

private:
    static std::string extractXml(const std::string &xml, const std::string &tag) {
        std::string open = "<" + tag + ">";
        std::string close = "</" + tag + ">";
        std::string cdata = "<" + tag + "><![CDATA[";
        std::string cdataEnd = "]]></" + tag + ">";

        // try CDATA first
        size_t a = xml.find(cdata);
        if (a != std::string::npos) {
            a += cdata.size();
            size_t b = xml.find(cdataEnd, a);
            if (b != std::string::npos) return xml.substr(a, b - a);
        }
        // plain
        a = xml.find(open);
        if (a != std::string::npos) {
            a += open.size();
            size_t b = xml.find(close, a);
            if (b != std::string::npos) return xml.substr(a, b - a);
        }
        return "";
    }

    static std::string buildTextReply(const std::string &to, const std::string &from,
                                       const std::string &content) {
        long long now = std::time(nullptr);
        std::ostringstream xml;
        xml << "<xml>"
            << "<ToUserName><![CDATA[" << to << "]]></ToUserName>"
            << "<FromUserName><![CDATA[" << from << "]]></FromUserName>"
            << "<CreateTime>" << now << "</CreateTime>"
            << "<MsgType><![CDATA[text]]></MsgType>"
            << "<Content><![CDATA[" << content << "]]></Content>"
            << "</xml>";
        return xml.str();
    }
};
