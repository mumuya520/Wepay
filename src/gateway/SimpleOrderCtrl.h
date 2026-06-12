#pragma once
#include <drogon/HttpController.h>
#include "../common/PayDb.h"
#include "../common/ChannelService.h"
#include "../common/EpaySign.h"
#include "../channel/ChannelPlugin.h"
#include <ctime>
#include <sstream>

using namespace drogon;

// 简单下单接口 - 无需签名验证
// POST /api/simple/create - 创建订单（用于测试和集成）
class SimpleOrderCtrl : public drogon::HttpController<SimpleOrderCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(SimpleOrderCtrl::create, "/api/simple/create", drogon::Post);
    METHOD_LIST_END

    // 简单下单接口 - 需要 API Key 认证
    void create(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        // 验证 API Key
        std::string apiKey = req->getHeader("X-API-Key");
        if (apiKey.empty()) {
            apiKey = req->getParameter("api_key");
        }
        
        auto &db = PayDb::instance();
        std::string validApiKey = db.getSetting("simple_order_api_key", "");
        
        if (validApiKey.empty() || apiKey != validApiKey) {
            auto resp = HttpResponse::newHttpResponse();
            resp->setContentTypeCode(CT_APPLICATION_JSON);
            resp->setStatusCode(drogon::k401Unauthorized);
            resp->setBody(R"({"code":-401,"msg":"API Key 无效或未配置"})");
            cb(resp);
            return;
        }

        auto j = req->getJsonObject();
        if (!j) {
            auto resp = HttpResponse::newHttpResponse();
            resp->setContentTypeCode(CT_APPLICATION_JSON);
            resp->setBody(R"({"code":-1,"msg":"参数格式错误"})");
            cb(resp);
            return;
        }

        // 提取参数
        std::string mchIdStr   = (*j).get("mch_id", "").asString();
        std::string outTradeNo = (*j).get("out_trade_no", "").asString();
        std::string amountStr  = (*j).get("amount", "").asString();
        std::string subject    = (*j).get("subject", "").asString();
        std::string payType    = (*j).get("pay_type", "").asString();
        std::string notifyUrl  = (*j).get("notify_url", "").asString();
        std::string returnUrl  = (*j).get("return_url", "").asString();

        // 参数验证
        if (mchIdStr.empty() || outTradeNo.empty() || amountStr.empty() || 
            subject.empty() || payType.empty()) {
            auto resp = HttpResponse::newHttpResponse();
            resp->setContentTypeCode(CT_APPLICATION_JSON);
            resp->setBody(R"({"code":-1,"msg":"参数不完整"})");
            cb(resp);
            return;
        }

        // 查询商户
        PayDb::Row mch;
        bool isNumericId = !mchIdStr.empty() && std::all_of(mchIdStr.begin(), mchIdStr.end(), ::isdigit);
        if (isNumericId) {
            mch = db.queryOne("SELECT * FROM merchant WHERE id=? AND state=1", {mchIdStr});
        }
        if (mch.empty()) {
            mch = db.queryOne("SELECT * FROM merchant WHERE mch_no=? AND state=1", {mchIdStr});
        }
        if (mch.empty()) {
            auto resp = HttpResponse::newHttpResponse();
            resp->setContentTypeCode(CT_APPLICATION_JSON);
            resp->setBody(R"({"code":-2,"msg":"商户不存在或已禁用"})");
            cb(resp);
            return;
        }

        int mchId = std::stoi(mch["id"]);

        // 金额验证
        double amount = 0;
        try { amount = std::stod(amountStr); } catch (...) {}
        if (amount <= 0) {
            auto resp = HttpResponse::newHttpResponse();
            resp->setContentTypeCode(CT_APPLICATION_JSON);
            resp->setBody(R"({"code":-5,"msg":"金额错误"})");
            cb(resp);
            return;
        }

        // 商户订单号去重
        auto exist = db.queryOne(
            "SELECT id FROM pay_order WHERE mch_id=? AND mch_order_no=?",
            {std::to_string(mchId), outTradeNo});
        if (!exist.empty()) {
            auto resp = HttpResponse::newHttpResponse();
            resp->setContentTypeCode(CT_APPLICATION_JSON);
            resp->setBody(R"({"code":-6,"msg":"商户订单号已存在"})");
            cb(resp);
            return;
        }

        // 选择通道
        auto channel = ChannelService::selectChannel(mchId, payType, amount);
        if (channel.channelId == 0) {
            auto resp = HttpResponse::newHttpResponse();
            resp->setContentTypeCode(CT_APPLICATION_JSON);
            resp->setBody(R"({"code":-7,"msg":"暂无可用支付通道"})");
            cb(resp);
            return;
        }

        // 计算费率
        double mchRate    = ChannelService::getMchRate(mchId, channel.channelId);
        double mchFee     = ChannelService::calcFee(amount, mchRate);
        double channelFee = ChannelService::calcFee(amount, channel.rate);

        // 生成订单号
        std::string orderId = ChannelService::generateOrderId(
            db.getSetting("order_prefix", "W"));

        long long now = std::time(nullptr);
        int closeMin = 5;
        try { closeMin = std::stoi(db.getSetting("close_minutes", "5")); } catch (...) {}
        long long expireTime = now + closeMin * 60;

        // 使用商户默认通知地址回退
        if (notifyUrl.empty()) notifyUrl = mch.count("notify_url") ? mch["notify_url"] : "";
        if (returnUrl.empty()) returnUrl = mch.count("return_url") ? mch["return_url"] : "";
        if (notifyUrl.empty()) notifyUrl = "http://localhost:8088/api/test/notify";
        if (returnUrl.empty()) returnUrl = "http://localhost:8088/api/test/return";

        std::string clientIp = req->getPeerAddr().toIp();

        // 创建订单
        bool ok = db.exec(
            "INSERT INTO pay_order("
            "order_id,mch_id,mch_order_no,channel_id,pay_type,amount,mch_fee,channel_fee,"
            "subject,notify_url,return_url,client_ip,state,expire_time,created_at,updated_at"
            ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,0,?,?,?)",
            {orderId, std::to_string(mchId), outTradeNo, std::to_string(channel.channelId),
             payType, ChannelService::fmtAmount(amount), ChannelService::fmtAmount(mchFee),
             ChannelService::fmtAmount(channelFee), subject, notifyUrl, returnUrl, clientIp,
             std::to_string(expireTime), std::to_string(now), std::to_string(now)});

        if (!ok) {
            LOG_ERROR << "Failed to create pay_order: orderId=" << orderId << " mchId=" << mchId 
                      << " amount=" << amount << " payType=" << payType;
            auto resp = HttpResponse::newHttpResponse();
            resp->setContentTypeCode(CT_APPLICATION_JSON);
            resp->setBody(R"({"code":-8,"msg":"订单创建失败"})");
            cb(resp);
            return;
        }
        
        LOG_INFO << "Order created: orderId=" << orderId << " mchId=" << mchId << " amount=" << amount;

        // 调用支付通道
        std::string payUrl, qrCode, channelOrderNo, rawResponse;
        
        auto plugin = ChannelPluginRegistry::instance().create(channel.plugin);
        if (plugin) {
            ChannelOrderRequest creq;
            creq.orderId = orderId;
            creq.mchOrderNo = outTradeNo;
            creq.amount = amount;
            creq.subject = subject;
            creq.notifyUrl = notifyUrl;
            creq.returnUrl = returnUrl;
            creq.clientIp = clientIp;
            creq.payType = payType;
            
            // 获取通道配置参数
            auto channelRow = db.queryOne("SELECT params_json FROM pay_channel WHERE id=?", {std::to_string(channel.channelId)});
            if (!channelRow.empty() && channelRow.count("params_json")) {
                try {
                    Json::CharReaderBuilder builder;
                    std::string errs;
                    std::istringstream iss(channelRow["params_json"]);
                    Json::parseFromStream(builder, iss, &creq.channelParams, &errs);
                } catch (...) {}
            }

            auto cres = plugin->createOrder(creq);
            if (cres.success) {
                payUrl = cres.payUrl;
                qrCode = cres.qrCode;
                channelOrderNo = cres.channelOrderNo;
                rawResponse = cres.rawResponse;
                
                db.exec("UPDATE pay_order SET pay_url=?,qrcode=?,channel_order_no=?,raw_response=?,updated_at=? WHERE order_id=?",
                        {payUrl, qrCode, channelOrderNo, rawResponse, std::to_string(std::time(nullptr)), orderId});
            } else {
                auto resp = HttpResponse::newHttpResponse();
                resp->setContentTypeCode(CT_APPLICATION_JSON);
                resp->setBody(R"({"code":-9,"msg":")" + cres.errMsg + R"("})");
                cb(resp);
                return;
            }
        } else {
            auto resp = HttpResponse::newHttpResponse();
            resp->setContentTypeCode(CT_APPLICATION_JSON);
            resp->setBody(R"({"code":-9,"msg":"支付通道插件不存在"})");
            cb(resp);
            return;
        }

        // 返回成功响应
        auto resp = HttpResponse::newHttpResponse();
        resp->setContentTypeCode(CT_APPLICATION_JSON);
        
        Json::Value data;
        data["order_id"] = orderId;
        data["mch_id"] = mch["mch_no"];
        data["out_trade_no"] = outTradeNo;
        data["amount"] = ChannelService::fmtAmount(amount);
        data["subject"] = subject;
        data["pay_type"] = payType;
        data["qr_code"] = qrCode;
        data["pay_url"] = payUrl;

        Json::Value result;
        result["code"] = 0;
        result["msg"] = "success";
        result["data"] = data;

        resp->setBody(result.toStyledString());
        cb(resp);
    }
};
