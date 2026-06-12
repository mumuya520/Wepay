// WePay-Cpp — 统一支付网关
// POST /gateway/create      统一下单
// POST /gateway/query        查询订单
// POST /gateway/close        关闭订单
// POST /gateway/refund       申请退款
// GET  /gateway/cashier/:id  收银台页面
#pragma once
#include <drogon/HttpController.h>
#include <ctime>
#include <cmath>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <map>
#include <algorithm>
#include "../common/AjaxResult.h"
#include "../common/PayDb.h"
#include "../common/EpaySign.h"
#include "../common/Md5Utils.h"
#include "../common/ChannelService.h"
#include "../common/NotifyTaskService.h"
#include "../common/RiskControlService.h"
#include "../common/WsBus.h"
#include "../common/AppContext.h"
#include <qrencode.h>
#include "../channel/ChannelPlugin.h"
#include "../channel/SmsReceiptPlugin.h"
#include "../common/SmtpUtils.h"
#include "../channel/v3/EmailService.h"
#include "../alipay/AlipayLib.h"
#include "../common/SyncHttp.h"
#include "../common/HttpCaller.h"

class GatewayCtrl : public drogon::HttpController<GatewayCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(GatewayCtrl::corsOptions, "/gateway/create",    drogon::Options);
        ADD_METHOD_TO(GatewayCtrl::corsOptions, "/gateway/query",     drogon::Options);
        ADD_METHOD_TO(GatewayCtrl::corsOptions, "/gateway/close",     drogon::Options);
        ADD_METHOD_TO(GatewayCtrl::corsOptions, "/gateway/refund",    drogon::Options);
        ADD_METHOD_TO(GatewayCtrl::create,      "/gateway/create",        drogon::Post);
        ADD_METHOD_TO(GatewayCtrl::createPhp,   "/gateway/create-wepay",  drogon::Post);
        ADD_METHOD_TO(GatewayCtrl::query,       "/gateway/query",         drogon::Post);
        ADD_METHOD_TO(GatewayCtrl::close,       "/gateway/close",         drogon::Post);
        ADD_METHOD_TO(GatewayCtrl::refund,      "/gateway/refund",        drogon::Post);
        ADD_METHOD_TO(GatewayCtrl::syncStatus,  "/gateway/sync-status",   drogon::Post);
        ADD_METHOD_TO(GatewayCtrl::cashier,     "/gateway/cashier/{1}",      drogon::Get);
        ADD_METHOD_TO(GatewayCtrl::cashierData, "/gateway/cashier-data/{1}", drogon::Get);
        ADD_METHOD_TO(GatewayCtrl::cashierQr,   "/gateway/qr/{1}",          drogon::Get);
        ADD_METHOD_TO(GatewayCtrl::cashierState, "/gateway/state/{1}",       drogon::Get);
        ADD_METHOD_TO(GatewayCtrl::getQrcode,   "/qrcodes/{1}",             drogon::Get);
        ADD_METHOD_TO(GatewayCtrl::manualNotify, "/gateway/notify/manual",   drogon::Get);
        // 反向代理 PHP 支付宝回调（使用独立路由，不影响其他插件）
        ADD_METHOD_TO(GatewayCtrl::proxyNotify, "/alipay/notify", drogon::Post, drogon::Get);
        // 易支付兼容
        ADD_METHOD_TO(GatewayCtrl::submitGet,  "/submit.php",  drogon::Get);
        ADD_METHOD_TO(GatewayCtrl::submitPost, "/submit.php",  drogon::Post);
        ADD_METHOD_TO(GatewayCtrl::mapi,       "/mapi.php",    drogon::Post);
        ADD_METHOD_TO(GatewayCtrl::apiQuery,   "/api.php",     drogon::Get);
        ADD_METHOD_TO(GatewayCtrl::apiQueryPost,"/api.php",     drogon::Post);
        // 用户手动确认收款(免签)
        ADD_METHOD_TO(GatewayCtrl::confirmPay,  "/gateway/confirm/{1}", drogon::Post);
        ADD_METHOD_TO(GatewayCtrl::confirmAction, "/gateway/confirm/{1}", drogon::Get);
        // 微信点金计划 iframe 页
        ADD_METHOD_TO(GatewayCtrl::goldPage,    "/gold.php",    drogon::Get);
    METHOD_LIST_END

    // CORS 预检请求
    void corsOptions(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->addHeader("Access-Control-Allow-Origin", "*");
        resp->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        resp->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
        resp->setStatusCode(drogon::HttpStatusCode::k200OK);
        cb(resp);
    }

    // ══════════════════════════════════════════════════════════
    //  统一下单 POST /gateway/create
    //  参数: mch_id, out_trade_no, pay_type, amount, subject,
    //        notify_url, return_url, param, sign, [sign_type]
    // ══════════════════════════════════════════════════════════
    void create(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto params = EpaySign::paramsFromRequest(req);
        auto get = [&](const std::string &k) -> std::string {
            auto it = params.find(k); return it == params.end() ? "" : it->second;
        };

        std::string mchIdStr   = get("mch_id");
        std::string outTradeNo = get("out_trade_no");
        std::string payType    = get("pay_type");
        std::string amountStr  = get("amount");
        std::string subject    = get("subject");
        std::string notifyUrl  = get("notify_url");
        std::string returnUrl  = get("return_url");
        std::string notifyEmail= get("notify_email");
        std::string param      = get("param");
        std::string sign       = get("sign");
        std::string body       = get("body");

        // 支付宝转账/红包不需要商户验证
        bool isAlipayDirect = (payType == "alipay_scan" || payType == "alipay_transfer" || payType == "alipay_redpacket");
        
        if (outTradeNo.empty() || payType.empty() || amountStr.empty()) {
            gatewayResp(cb, -1, "参数不完整"); return;
        }

        auto &db = PayDb::instance();
        PayDb::Row mch;
        int mchId = 0;
        std::string mchKey = "";
        
        // 支付宝直接转账/红包：使用虚拟商户
        if (isAlipayDirect) {
            mchId = 0;  // 虚拟商户ID
            mchKey = "";  // 无需验证签名
        } else {
            // 其他支付类型需要验证商户
            if (mchIdStr.empty() || sign.empty()) {
                gatewayResp(cb, -1, "参数不完整"); return;
            }
            
            // 查商户 (mch_id 可能是数字ID 或 字母开头的商户编号)
            bool isNumericId = !mchIdStr.empty() && std::all_of(mchIdStr.begin(), mchIdStr.end(), ::isdigit);
            if (isNumericId) {
                mch = db.queryOne("SELECT * FROM merchant WHERE id=? AND state=1", {mchIdStr});
            }
            if (mch.empty()) {
                mch = db.queryOne("SELECT * FROM merchant WHERE mch_no=? AND state=1", {mchIdStr});
            }
            if (mch.empty()) { gatewayResp(cb, -2, "商户不存在或已禁用"); return; }
            
            mchId = std::stoi(mch["id"]);
            mchKey = mch["mch_key"];
        }

        std::string publicKey = "";
        
        // 非支付宝直接转账/红包需要验证签名和IP白名单
        if (!isAlipayDirect) {
            publicKey = mch.count("public_key") ? mch["public_key"] : "";
            
            // IP 白名单校验
            std::string ipWhite = mch.count("ip_white") ? mch["ip_white"] : "";
            if (!ipWhite.empty()) {
                std::string clientIp = req->getPeerAddr().toIp();
                if (ipWhite.find(clientIp) == std::string::npos) {
                    gatewayResp(cb, -3, "IP不在白名单中"); return;
                }
            }

            // 验签（支持 MD5 和 RSA）
            if (!EpaySign::verify(params, mchKey, sign, publicKey)) {
                gatewayResp(cb, -4, "签名错误"); return;
            }
        }

        double amount = 0;
        try { amount = std::stod(amountStr); } catch (...) {}
        if (amount <= 0) { gatewayResp(cb, -5, "金额错误"); return; }

        // 商户订单号去重（支付宝直接转账/红包使用虚拟商户，跳过去重）
        if (!isAlipayDirect) {
            auto exist = db.queryOne(
                "SELECT id FROM pay_order WHERE mch_id=? AND mch_order_no=?",
                {std::to_string(mchId), outTradeNo});
            if (!exist.empty()) { gatewayResp(cb, -6, "商户订单号已存在"); return; }
        }

        // 选择通道（支付宝直接转账/红包不需要通道）
        int channelId = 0;
        double mchRate = 0;
        double mchFee = 0;
        double channelFee = 0;
        std::string channelPlugin = "";
        
        if (!isAlipayDirect) {
            auto channel = ChannelService::selectChannel(mchId, payType, amount);
            if (channel.channelId == 0) {
                gatewayResp(cb, -7, "暂无可用支付通道"); return;
            }
            
            channelId = channel.channelId;
            channelPlugin = channel.plugin;
            // 计算费率
            mchRate    = ChannelService::getMchRate(mchId, channel.channelId);
            mchFee     = ChannelService::calcFee(amount, mchRate);
            channelFee = ChannelService::calcFee(amount, channel.rate);
        }

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
        // 仍为空 -> 兜底回到当前站点的内置测试接收端
        if (notifyUrl.empty()) notifyUrl = requestBaseUrl(req) + "/api/test/notify";
        if (returnUrl.empty()) returnUrl = requestBaseUrl(req) + "/api/test/return";

        std::string clientIp = req->getPeerAddr().toIp();

        // 获取用户标识（用于区分管理员和商户的订单）
        std::string userId = get("user_id");  // 可选参数
        std::string userType = get("user_type");  // admin 或 merchant
        if (userType.empty()) userType = "merchant";  // 默认为商户
        std::string buyerId = userType + ":" + userId;  // 格式: "admin:123" 或 "merchant:456"

        bool ok = db.exec(
            "INSERT INTO pay_order(order_id,mch_id,app_id,mch_order_no,channel_id,"
            "pay_type,amount,real_amount,mch_fee_rate,mch_fee_amount,"
            "channel_fee_rate,channel_fee_amount,subject,body,param,"
            "notify_url,return_url,notify_email,client_ip,state,notify_state,"
            "expire_time,created_at,updated_at,buyer_id) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,0,0,?,?,?,?)",
            {orderId, std::to_string(mchId), "", outTradeNo,
             std::to_string(channelId), payType,
             ChannelService::fmtAmount(amount), ChannelService::fmtAmount(amount),
             ChannelService::fmtAmount(mchRate), ChannelService::fmtAmount(mchFee),
             ChannelService::fmtAmount(0), ChannelService::fmtAmount(channelFee),
             subject, body, param, notifyUrl, returnUrl, notifyEmail, clientIp,
             std::to_string(expireTime), std::to_string(now), std::to_string(now), buyerId});

        if (!ok) { gatewayResp(cb, -8, "订单创建失败"); return; }

        // 支付宝特殊处理（transfer/redpacket/scan）
        if (payType == "alipay_transfer" || payType == "alipay_redpacket" || payType == "alipay_scan") {
            std::string scheme = "";
            if (payType == "alipay_scan") {
                // alipay_scan 和 alipay_transfer 共用 AlipayLib::generateTransferScheme
            }

            std::string alipayUserId = get("alipay_user_id");
            std::string alipayUserName = get("alipay_user_name");
            std::string alipayLoginId = get("alipay_login_id");
            std::string memo = get("memo");
            std::string remark = get("remark");
            
            if (alipayUserId.empty()) {
                db.exec("UPDATE pay_order SET state=-1,updated_at=? WHERE order_id=?",
                        {std::to_string(std::time(nullptr)), orderId});
                gatewayResp(cb, -9, "缺少支付宝用户ID"); return;
            }
            
            // 生成支付宝 Scheme（使用 C++ AlipayLib）
            if (payType == "alipay_transfer") {
                std::string transferMemo = memo.empty() ? "" : memo;
                scheme = AlipayLib::generateTransferScheme(alipayUserId, amountStr, transferMemo);
            } else {
                std::string redpacketRemark = remark.empty() ? "" : remark;
                scheme = AlipayLib::generateRedPacketScheme(alipayLoginId, alipayUserId, alipayUserName, amountStr, redpacketRemark);
            }
            
            if (scheme.empty()) {
                db.exec("UPDATE pay_order SET state=-1,updated_at=? WHERE order_id=?",
                        {std::to_string(std::time(nullptr)), orderId});
                gatewayResp(cb, -9, "生成支付宝Scheme失败"); return;
            }
            
            // 保存 Scheme 到 channel_data
            Json::Value channelData;
            channelData["scheme"] = scheme;
            channelData["type"] = payType;
            channelData["alipay_user_id"] = alipayUserId;
            if (!alipayUserName.empty()) channelData["alipay_user_name"] = alipayUserName;
            if (!memo.empty()) channelData["memo"] = memo;
            if (!remark.empty()) channelData["remark"] = remark;
            
            db.exec("UPDATE pay_order SET pay_url=?,channel_data=?,updated_at=? WHERE order_id=?",
                    {scheme, channelData.toStyledString(), std::to_string(std::time(nullptr)), orderId});
            
            // 返回支付信息
            Json::Value result;
            result["code"] = 1;
            result["msg"] = "订单创建成功";
            result["data"]["order_id"] = orderId;
            result["data"]["scheme"] = scheme;
            result["data"]["amount"] = amountStr;
            result["data"]["pay_url"] = "/gateway/cashier/" + orderId;  // 支付页面
            result["data"]["qr_url"] = "https://chart.googleapis.com/chart?chs=300x300&chld=L|0&cht=qr&chl=" + 
                                       urlEncode(scheme);
            
            auto resp = drogon::HttpResponse::newHttpJsonResponse(result);
            resp->setStatusCode(drogon::k200OK);
            cb(resp);
            return;
        }

        // 其他支付类型：调用通道插件
        if (!isAlipayDirect) {
            Json::Value channelParams;
            auto channel = ChannelService::selectChannel(mchId, payType, amount);
            if (!channel.paramsJson.empty()) {
                Json::Reader reader;
                reader.parse(channel.paramsJson, channelParams);
            }
            channelParams["plugin_code"] = channel.plugin;
            channelParams["channel_id"] = channel.channelId;
            channelParams["mch_id"] = mchId;
            auto plugin = ChannelPluginRegistry::instance().create(channel.plugin);
            if (plugin) {
                ChannelOrderRequest creq;
                creq.orderId = orderId;
                creq.mchOrderNo = outTradeNo;
                creq.amount = amount;
                creq.subject = subject;
                creq.body = body;
                creq.notifyUrl = requestBaseUrl(req) + "/notify/channel/" + channel.plugin;
                creq.returnUrl = returnUrl;
                creq.clientIp = clientIp;
                creq.payType = payType;
                creq.channelParams = channelParams;
                auto cres = plugin->createOrder(creq);
                if (!cres.success) {
                    db.exec("UPDATE pay_order SET state=-1,updated_at=? WHERE order_id=?",
                            {std::to_string(std::time(nullptr)), orderId});
                    gatewayResp(cb, -9, "通道下单失败: " + cres.errMsg); return;
                }
                db.exec("UPDATE pay_order SET pay_url=?,qrcode=?,channel_order_no=?,raw_response=?,updated_at=? WHERE order_id=?",
                        {cres.payUrl, cres.qrCode, cres.channelOrderNo, cres.rawResponse,
                         std::to_string(std::time(nullptr)), orderId});
            }

            // wepay_v3：订单创建后用 EmailService（HTML 模板 + SmtpUtils）发邮件
            if (channel.plugin == "wepay_v3") {
            std::string toEmail = notifyEmail;
            if (toEmail.empty()) {
                try {
                    auto fr = db.queryOne("SELECT from_email FROM v3_email_account WHERE status=1 ORDER BY id ASC LIMIT 1", {});
                    if (!fr.empty()) toEmail = fr["from_email"];
                } catch (...) {}
            }
            if (!toEmail.empty()) {
                std::string base = requestBaseUrl(req); // site_url > config.json > X-Forwarded-Proto+Host
                std::string mchIdStr = std::to_string(mchId);
                wepay::v3::EmailService::EmailData ed;
                ed.orderId         = orderId;
                ed.merchantOrderId = outTradeNo;
                ed.merchantId      = mchIdStr;
                ed.toEmail         = toEmail;
                ed.money           = amountStr;
                ed.payType         = payType;
                ed.createTime      = std::to_string(now);
                ed.callbackUrl     = base + "/api/wepay/v3/callback/manual?order_id=" + orderId + "&mch_id=" + mchIdStr;
                ed.resendEmailUrl  = base + "/api/wepay/v3/email/resend?order_id=" + orderId + "&mch_id=" + mchIdStr;
                ed.orderViewUrl    = base + "/admin/#/v3/orders?q=" + orderId;
                ed.closeOrderUrl   = base + "/gateway/close";
                ed.editOrderUrl    = base + "/admin/#/v3/orders?q=" + orderId;
                ed.deleteOrderUrl  = base + "/admin/#/v3/orders?q=" + orderId;
                ed.toggleChannelUrl= base + "/admin/#/channel";
                ed.statisticUrl    = base + "/admin/#/dashboard";
                auto svc = wepay::v3::EmailService::globalInstance();
                if (!svc) {
                    wepay::v3::EmailService::EmailConfig empty;
                    svc = std::make_shared<wepay::v3::EmailService>(empty);
                }
                svc->sendPayFailEmail(ed);
            }
            }
        }

        // 返回支付信息
        Json::Value data;
        data["code"]          = 1;
        data["msg"]           = "订单创建成功";
        data["trade_no"]      = orderId;
        data["out_trade_no"]  = outTradeNo;
        data["pay_type"]      = payType;
        data["amount"]        = amountStr;
        data["cashier_url"]   = "/gateway/cashier/" + orderId;
        data["pay_url"]       = "/gateway/cashier/" + orderId;
        data["notify_url"]    = notifyUrl;
        data["return_url"]    = returnUrl;
        data["expire_time"]   = (Json::Int64)expireTime;
        cb(drogon::HttpResponse::newHttpJsonResponse(data));
    }

    // ══════════════════════════════════════════════════════════
    //  查询订单 POST /gateway/query
    // ══════════════════════════════════════════════════════════
    void query(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto params = EpaySign::paramsFromRequest(req);
        auto get = [&](const std::string &k) -> std::string {
            auto it = params.find(k); return it == params.end() ? "" : it->second;
        };

        std::string mchIdStr   = get("mch_id");
        std::string tradeNo    = get("trade_no");
        std::string outTradeNo = get("out_trade_no");
        std::string sign       = get("sign");

        auto &db = PayDb::instance();
        auto mch = findMch(db, mchIdStr);
        if (mch.empty()) { gatewayResp(cb, -2, "商户不存在"); return; }
        if (!EpaySign::verify(params, mch["mch_key"], sign)) {
            gatewayResp(cb, -4, "签名错误"); return;
        }

        PayDb::Row order;
        if (!tradeNo.empty())
            order = db.queryOne("SELECT * FROM pay_order WHERE order_id=? AND mch_id=?",
                                {tradeNo, mch["id"]});
        else if (!outTradeNo.empty())
            order = db.queryOne("SELECT * FROM pay_order WHERE mch_order_no=? AND mch_id=?",
                                {outTradeNo, mch["id"]});

        if (order.empty()) { gatewayResp(cb, -1, "订单不存在"); return; }

        Json::Value data;
        data["code"]          = 1;
        data["trade_no"]      = order["order_id"];
        data["out_trade_no"]  = order["mch_order_no"];
        data["pay_type"]      = order["pay_type"];
        data["amount"]        = order["amount"];
        data["real_amount"]   = order["real_amount"];
        data["status"]        = std::stoi(order["state"]);
        data["pay_time"]      = order.count("pay_time") ? order["pay_time"] : "0";
        cb(drogon::HttpResponse::newHttpJsonResponse(data));
    }

    // ══════════════════════════════════════════════════════════
    //  关闭订单 POST /gateway/close
    // ══════════════════════════════════════════════════════════
    void close(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto params = EpaySign::paramsFromRequest(req);
        auto get = [&](const std::string &k) -> std::string {
            auto it = params.find(k); return it == params.end() ? "" : it->second;
        };

        auto &db = PayDb::instance();
        auto mch = findMch(db, get("mch_id"));
        if (mch.empty()) { gatewayResp(cb, -2, "商户不存在"); return; }
        if (!EpaySign::verify(params, mch["mch_key"], get("sign"))) {
            gatewayResp(cb, -4, "签名错误"); return;
        }

        std::string tradeNo    = get("trade_no");
        std::string outTradeNo = get("out_trade_no");
        PayDb::Row order;
        if (!tradeNo.empty())
            order = db.queryOne("SELECT * FROM pay_order WHERE order_id=? AND mch_id=?",
                                {tradeNo, mch["id"]});
        else if (!outTradeNo.empty())
            order = db.queryOne("SELECT * FROM pay_order WHERE mch_order_no=? AND mch_id=?",
                                {outTradeNo, mch["id"]});

        if (order.empty()) { gatewayResp(cb, -1, "订单不存在"); return; }
        if (order["state"] != "0") { gatewayResp(cb, -3, "只能关闭未支付的订单"); return; }

        long long now = std::time(nullptr);
        db.exec("UPDATE pay_order SET state=-1,updated_at=? WHERE order_id=?",
                {std::to_string(now), order["order_id"]});
        db.exec("DELETE FROM tmp_price WHERE oid=?", {order["order_id"]});
        gatewayResp(cb, 1, "关闭成功");
    }

    // ══════════════════════════════════════════════════════════
    //  申请退款 POST /gateway/refund
    // ══════════════════════════════════════════════════════════
    void refund(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto params = EpaySign::paramsFromRequest(req);
        auto get = [&](const std::string &k) -> std::string {
            auto it = params.find(k); return it == params.end() ? "" : it->second;
        };

        auto &db = PayDb::instance();
        auto mch = findMch(db, get("mch_id"));
        if (mch.empty()) { gatewayResp(cb, -2, "商户不存在"); return; }
        if (!EpaySign::verify(params, mch["mch_key"], get("sign"))) {
            gatewayResp(cb, -4, "签名错误"); return;
        }

        std::string tradeNo      = get("trade_no");
        std::string outTradeNo   = get("out_trade_no");
        std::string refundAmount = get("refund_amount");
        std::string reason       = get("reason");
        std::string mchRefundNo  = get("mch_refund_no");

        PayDb::Row order;
        if (!tradeNo.empty())
            order = db.queryOne("SELECT * FROM pay_order WHERE order_id=? AND mch_id=?",
                                {tradeNo, mch["id"]});
        else if (!outTradeNo.empty())
            order = db.queryOne("SELECT * FROM pay_order WHERE mch_order_no=? AND mch_id=?",
                                {outTradeNo, mch["id"]});

        if (order.empty()) { gatewayResp(cb, -1, "订单不存在"); return; }
        // 允许已支付(1)和已冻结(3)订单退款
        if (order["state"] != "1" && order["state"] != "3") {
            gatewayResp(cb, -3, "当前订单状态不支持退款"); return;
        }

        double rAmount = 0;
        try { rAmount = std::stod(refundAmount); } catch (...) {}
        double orderAmount = 0;
        try { orderAmount = std::stod(order["amount"]); } catch (...) {}
        double existRefund = 0;
        try { existRefund = std::stod(order.count("refund_amount") ? order["refund_amount"] : "0"); } catch (...) {}
        double refundable = orderAmount - existRefund;
        if (rAmount <= 0) rAmount = refundable;  // 默认退全部剩余
        if (rAmount > refundable) { gatewayResp(cb, -5, "退款金额超过可退金额"); return; }

        // 按比例计算应退手续费
        double mchFeeRate = 0;
        try { mchFeeRate = std::stod(order.count("mch_fee_rate") ? order["mch_fee_rate"] : "0"); } catch (...) {}
        double refundFee = ChannelService::calcFee(rAmount, mchFeeRate);
        // 实际扣除 = 退款金额 - 退还手续费 (net cost to merchant)
        double mchDeduct = rAmount - refundFee;
        if (mchDeduct < 0) mchDeduct = 0;

        std::string refundNo = ChannelService::generateRefundNo();
        long long now = std::time(nullptr);
        int mchId = std::stoi(mch["id"]);

        db.exec("INSERT INTO refund_order(refund_no,order_id,mch_id,mch_refund_no,"
                "channel_id,pay_type,pay_amount,refund_amount,refund_fee,reason,state,created_at,updated_at) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,0,?,?)",
                {refundNo, order["order_id"], std::to_string(mchId), mchRefundNo,
                 order["channel_id"], order["pay_type"], order["amount"],
                 ChannelService::fmtAmount(rAmount), ChannelService::fmtAmount(refundFee),
                 reason, std::to_string(now), std::to_string(now)});

        // 更新订单退款金额；仅全额退款时设 state=2
        double newRefundTotal = existRefund + rAmount;
        std::string newState = (newRefundTotal >= orderAmount - 0.001) ? "2" : order["state"];
        db.exec("UPDATE pay_order SET refund_amount=?,state=?,updated_at=? WHERE order_id=?",
                {ChannelService::fmtAmount(newRefundTotal), newState,
                 std::to_string(now), order["order_id"]});

        // 扣除商户余额(退款金额 - 退还手续费)
        ChannelService::changeMchBalance(mchId, 6, mchDeduct, "refund", refundNo, "退款");

        Json::Value data;
        data["code"]       = 1;
        data["msg"]        = "退款申请已受理";
        data["refund_no"]  = refundNo;
        data["refund_amount"] = ChannelService::fmtAmount(rAmount);
        data["refund_fee_return"] = ChannelService::fmtAmount(refundFee);
        data["total_refunded"] = ChannelService::fmtAmount(newRefundTotal);
        cb(drogon::HttpResponse::newHttpJsonResponse(data));
    }

    // ══════════════════════════════════════════════════════════
    //  手动确认收款 POST /gateway/confirm/{orderId}
    //  用户在收银台提交支付宝/微信交易订单号
    // ══════════════════════════════════════════════════════════
    void confirmPay(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                    std::string orderId) {
        std::string tradeNo;
        auto body = req->getJsonObject();
        if (body) {
            tradeNo = (*body).get("trade_no", "").asString();
        } else {
            tradeNo = req->getParameter("trade_no");
        }

        std::string msg;
        int ret = SmsReceiptPlugin::userConfirm(orderId, tradeNo, msg);

        Json::Value r;
        r["code"] = ret <= 0 ? ret : 1;
        r["msg"]  = msg;
        if (ret == 0) r["state"] = 1;       // 已支付
        else if (ret == 1) r["state"] = 0;   // 待审核
        else r["state"] = -1;                 // 失败
        cb(drogon::HttpResponse::newHttpJsonResponse(r));
    }

    // ══════════════════════════════════════════════════════════
    //  管理员邮件确认/拒绝 GET /gateway/confirm/{orderId}?token=xxx&action=confirm|reject
    // ══════════════════════════════════════════════════════════
    void confirmAction(const drogon::HttpRequestPtr &req,
                       std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                       std::string orderId) {
        std::string token  = req->getParameter("token");
        std::string action = req->getParameter("action");

        if (token.empty() || action.empty()) {
            auto r = drogon::HttpResponse::newHttpResponse();
            r->setStatusCode(drogon::k400BadRequest);
            r->setContentTypeCode(drogon::CT_TEXT_HTML);
            r->setBody("<h2>参数不完整</h2><p>缺少 token 或 action</p>");
            cb(r); return;
        }

        std::string msg;
        int ret = SmsReceiptPlugin::adminAction(orderId, token, action, msg);

        // 返回简洁 HTML 结果页
        std::string color = (ret == 0 && action == "confirm") ? "#10b981" :
                            (ret == 0 && action == "reject")  ? "#ef4444" : "#f59e0b";
        std::string icon  = (ret == 0 && action == "confirm") ? "&#10004;" :
                            (ret == 0 && action == "reject")  ? "&#10008;" : "&#9888;";
        std::string html =
            "<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>" + msg + "</title>"
            "<style>body{font-family:-apple-system,sans-serif;display:flex;"
            "align-items:center;justify-content:center;min-height:100vh;"
            "background:#f5f7fa;margin:0}.card{text-align:center;background:#fff;"
            "padding:48px;border-radius:16px;box-shadow:0 4px 24px rgba(0,0,0,.08)}"
            ".icon{font-size:64px;color:" + color + "}"
            "h2{margin:16px 0 8px;color:#1a1a1a}"
            "p{color:#666;font-size:14px}</style></head>"
            "<body><div class='card'>"
            "<div class='icon'>" + icon + "</div>"
            "<h2>" + msg + "</h2>"
            "<p>订单: " + orderId + "</p>"
            "</div></body></html>";

        auto r = drogon::HttpResponse::newHttpResponse();
        r->setContentTypeCode(drogon::CT_TEXT_HTML);
        r->setBody(html);
        cb(r);
    }

    // ══════════════════════════════════════════════════════════
    //  收银台页面 GET /gateway/cashier/{orderId}
    // ══════════════════════════════════════════════════════════
    // 公开: 收银台轮询订单状态 (无需签名)
    // 优先查询 PHP 数据库（支付宝回调更新），如果没有则查询 C++ 数据库
    void cashierState(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                      std::string orderId) {
        auto &db = PayDb::instance();
        
        // 首先查询 C++ 数据库获取订单基本信息
        auto cppRow = db.queryOne(
            "SELECT state,pay_time,real_amount,return_url,expire_time,mch_order_no FROM pay_order WHERE order_id=?",
            {orderId});
        
        Json::Value r;
        if (cppRow.empty()) {
            r["state"] = -2; r["msg"] = "订单不存在";
            cb(drogon::HttpResponse::newHttpJsonResponse(r));
            return;
        }
        
        // 获取 mch_order_no（PHP 订单号）
        std::string mchOrderNo = cppRow.count("mch_order_no") ? cppRow["mch_order_no"] : "";
        
        // 如果有 PHP 订单号，查询 PHP 数据库获取最新状态
        int phpStatus = -1;
        if (!mchOrderNo.empty()) {
            try {
                // 使用同一个 PayDb 连接查询 PHP 订单表
                auto phpRow = db.queryOne(
                    "SELECT status FROM codepay_orders WHERE out_trade_no=? LIMIT 1",
                    {mchOrderNo});
                
                if (!phpRow.empty() && phpRow.count("status")) {
                    try { phpStatus = std::stoi(phpRow["status"]); } catch (...) {}
                }
            } catch (...) {
                // PHP 数据库查询失败，使用 C++ 数据库的状态
            }
        }
        
        // 如果 PHP 数据库显示已支付，更新 C++ 订单状态
        if (phpStatus == 1) {
            db.exec("UPDATE pay_order SET state=1,pay_time=? WHERE order_id=?",
                    {std::to_string(std::time(nullptr)), orderId});
            r["state"] = 1;
            r["pay_time"] = std::to_string(std::time(nullptr));
        } else {
            // 否则使用 C++ 数据库的状态
            int st = 0; try { st = std::stoi(cppRow["state"]); } catch (...) {}
            r["state"] = st;
            r["pay_time"] = cppRow.count("pay_time") ? cppRow["pay_time"] : "0";
        }
        
        r["real_amount"] = cppRow.count("real_amount") ? cppRow["real_amount"] : "0";
        r["return_url"] = cppRow.count("return_url") ? cppRow["return_url"] : "";
        long long expire = 0; try { expire = std::stoll(cppRow["expire_time"]); } catch (...) {}
        long long left = expire - std::time(nullptr);
        r["left_seconds"] = (Json::Int64)(left > 0 ? left : 0);
        
        cb(drogon::HttpResponse::newHttpJsonResponse(r));
    }

    // 公开: 根据订单生成二维码 SVG (使用该订单的 pay_url / qrcode)
    void cashierQr(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                   std::string orderId) {
        auto &db = PayDb::instance();
        auto row = db.queryOne("SELECT pay_url,qrcode FROM pay_order WHERE order_id=?", {orderId});
        std::string qrField;
        if (!row.empty()) {
            qrField = row.count("qrcode") ? row["qrcode"] : "";
            if (qrField.empty()) qrField = row.count("pay_url") ? row["pay_url"] : "";
        }

        // 1) 经营码 URL 模式：代理 PHP 图片
        if (!qrField.empty() && qrField.rfind("http", 0) == 0) {
            auto httpResp = SyncHttp::get(qrField);
            if (httpResp.success) {
                auto r = drogon::HttpResponse::newHttpResponse();
                r->setBody(httpResp.body);
                r->setContentTypeCode(drogon::CT_IMAGE_PNG);
                r->addHeader("Cache-Control", "no-store");
                cb(r);
                return;
            }
        }

        // 2) 经营码 data:image 模式：解码 base64 返回 PNG
        if (!qrField.empty() && qrField.rfind("data:", 0) == 0) {
            size_t comma = qrField.find(',');
            if (comma != std::string::npos) {
                std::string b64 = qrField.substr(comma + 1);
                std::string decoded;
                static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                for (size_t i = 0; i + 3 < b64.size(); ) {
                    unsigned a = strchr(tbl, b64[i]) - tbl;
                    unsigned b = strchr(tbl, b64[i+1]) - tbl;
                    unsigned c = strchr(tbl, b64[i+2]) - tbl;
                    unsigned d = strchr(tbl, b64[i+3]) - tbl;
                    decoded.push_back(char((a << 2) | (b >> 4)));
                    if (b64[i+2] != '=') decoded.push_back(char((b << 4) | (c >> 2)));
                    if (b64[i+3] != '=') decoded.push_back(char((c << 6) | d));
                    i += 4;
                }
                if (!decoded.empty()) {
                    auto r = drogon::HttpResponse::newHttpResponse();
                    r->setBody(decoded);
                    r->setContentTypeCode(drogon::CT_IMAGE_PNG);
                    r->addHeader("Cache-Control", "no-store");
                    cb(r);
                    return;
                }
            }
        }

        // 3) 以上都不是：把内容本身当链接生成二维码
        if (qrField.empty()) qrField = orderId;
        QRcode *qr = QRcode_encodeString(qrField.c_str(), 0, QR_ECLEVEL_M, QR_MODE_8, 1);
        if (!qr) { auto r = drogon::HttpResponse::newHttpResponse(); r->setStatusCode(drogon::k500InternalServerError); r->setBody("qr fail"); cb(r); return; }
        int w = qr->width, margin = 2, box = 8;
        int dim = (w + margin * 2) * box;
        std::ostringstream svg;
        svg << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << dim << "\" height=\"" << dim
            << "\" viewBox=\"0 0 " << dim << " " << dim << "\" shape-rendering=\"crispEdges\">"
            << "<rect width=\"100%\" height=\"100%\" fill=\"#fff\"/><path fill=\"#000\" d=\"";
        for (int y = 0; y < w; ++y)
            for (int x = 0; x < w; ++x)
                if (qr->data[y * w + x] & 1) {
                    int px = (x + margin) * box, py = (y + margin) * box;
                    svg << "M" << px << " " << py << "h" << box << "v" << box << "h-" << box << "z";
                }
        svg << "\"/></svg>";
        QRcode_free(qr);
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setBody(svg.str());
        resp->setContentTypeCode(drogon::CT_NONE);
        resp->removeHeader("content-type");
        resp->addHeader("Content-Type", "image/svg+xml; charset=utf-8");
        resp->addHeader("Cache-Control", "no-store");
        cb(resp);
    }

    void cashier(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                 std::string orderId) {
        // 检查订单是否存在且未过期
        auto &db = PayDb::instance();
        auto row = db.queryOne("SELECT state,expire_time FROM pay_order WHERE order_id=?", {orderId});
        auto make404 = [&](const std::string &msg) {
            std::string html = R"(<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>页面不存在</title><style>*{box-sizing:border-box;margin:0;padding:0}body{font-family:-apple-system,BlinkMacSystemFont,'PingFang SC',sans-serif;background:#f5f7fa;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px}.box{text-align:center;max-width:320px}.box svg{margin-bottom:24px}.box h2{font-size:20px;font-weight:700;color:#1a1a1a;margin-bottom:10px}.box p{font-size:14px;color:#999;line-height:1.7}</style></head><body><div class="box"><svg width="220" height="180" viewBox="0 0 220 180" fill="none" xmlns="http://www.w3.org/2000/svg"><ellipse cx="110" cy="155" rx="90" ry="18" fill="#dce8f5"/><rect x="70" y="90" width="80" height="55" rx="6" fill="#b8d4f0"/><rect x="76" y="96" width="68" height="43" rx="4" fill="#e8f2fc"/><rect x="84" y="104" width="20" height="3" rx="1.5" fill="#a0bcd8"/><rect x="84" y="111" width="30" height="3" rx="1.5" fill="#a0bcd8"/><rect x="84" y="118" width="24" height="3" rx="1.5" fill="#a0bcd8"/><circle cx="110" cy="78" r="22" fill="#f5c842"/><text x="110" y="85" text-anchor="middle" font-size="22" font-weight="900" fill="#fff" font-family="sans-serif">?</text><text x="38" y="60" font-size="36" font-weight="900" fill="#5b9bd5" font-family="sans-serif" opacity=".9">4</text><text x="86" y="48" font-size="36" font-weight="900" fill="#5b9bd5" font-family="sans-serif" opacity=".9">0</text><text x="152" y="60" font-size="36" font-weight="900" fill="#5b9bd5" font-family="sans-serif" opacity=".9">4</text></svg><h2>订单不存在或已过期</h2><p>)" + msg + R"(</p></div></body></html>)";
            auto r = drogon::HttpResponse::newHttpResponse();
            r->setStatusCode(drogon::k404NotFound);
            r->setContentTypeCode(drogon::CT_TEXT_HTML);
            r->setBody(html); cb(r);
        };
        if (row.empty()) { make404("该订单不存在，请确认链接是否正确"); return; }
        int state = 0;
        long long expireTime = 0;
        try { state = std::stoi(row["state"]); } catch (...) {}
        try { expireTime = std::stoll(row.count("expire_time") ? row["expire_time"] : "0"); } catch (...) {}
        if (state < 0 || (expireTime > 0 && std::time(nullptr) > expireTime)) {
            make404("订单已过期或已关闭<br>请重新发起支付"); return;
        }
        // 根据 templateId 选择主题页面
        // 自动向上查找 web/cashier/ 目录，部署根目录名称可能不同，但内部结构固定
        auto cfgDir = AppContext::instance().configDir();
        std::string tplId = PayDb::instance().getSetting("templateId", "default");

        // 从 config.json 所在目录向上查找 web/cashier/（最多向上 5 层）
        std::filesystem::path tplRoot;
        for (int up = 0; up <= 5; ++up) {
            auto candidate = cfgDir / "web" / "cashier";
            if (std::filesystem::exists(candidate)) {
                tplRoot = candidate;
                break;
            }
            cfgDir = cfgDir.parent_path();
        }
        if (tplRoot.empty()) {
            auto r = drogon::HttpResponse::newHttpResponse();
            r->setStatusCode(drogon::k500InternalServerError);
            r->setBody("web/cashier/ directory not found, please check deployment layout"); cb(r); return;
        }

        std::string tplFile;
        if (tplId == "default") {
            // config.json 所在目录（部署根目录）
            tplFile = "cashier.html";
        } else {
            tplFile = tplId + "/index.html";
        }

        auto p = tplRoot / tplFile;
        std::string tpl;
        if (std::filesystem::exists(p)) {
            std::ifstream f(p);
            tpl.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        if (tpl.empty()) {
            auto r = drogon::HttpResponse::newHttpResponse();
            r->setStatusCode(drogon::k500InternalServerError);
            r->setBody("template not found: " + tplFile); cb(r); return;
        }

        // 防 XSS: orderId 只允许字母数字下划线
        std::string safeId;
        for (char c : orderId) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')
                safeId += c;
        }
        if (safeId.empty()) {
            auto r = drogon::HttpResponse::newHttpResponse();
            r->setStatusCode(drogon::k400BadRequest);
            r->setBody("invalid order id"); cb(r); return;
        }
        std::string inject = "<script>window.__CASHIER_ORDER_ID__='" + safeId + "';</script>";
        std::string page = tpl;
        auto pos = page.find("</head>");
        if (pos != std::string::npos) page.insert(pos, inject);
        else page = inject + page;

        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setContentTypeCode(drogon::CT_TEXT_HTML);
        resp->setBody(std::move(page));
        cb(resp);
    }

    // ══════════════════════════════════════════════════════════
    //  收银台数据接口 GET /gateway/cashier-data/{orderId}
    //  返回 JSON 供 Vue 收银台页面使用
    // ══════════════════════════════════════════════════════════
    void cashierData(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                     std::string orderId) {
        auto &db = PayDb::instance();
        auto row = db.queryOne("SELECT * FROM pay_order WHERE order_id=?", {orderId});
        if (row.empty()) {
            Json::Value j; j["code"] = -1; j["msg"] = "order not found";
            auto r = drogon::HttpResponse::newHttpJsonResponse(j);
            r->setStatusCode(drogon::k404NotFound); cb(r); return;
        }
        int state = 0;
        try { state = std::stoi(row["state"]); } catch (...) {}

        std::string siteName = db.getSetting("siteName", "WePay");
        long long expireTs = 0;
        try { expireTs = std::stoll(row.count("expire_time") ? row["expire_time"] : "0"); } catch (...) {}
        long long nowTs = std::time(nullptr);
        long long leftSec = expireTs > nowTs ? expireTs - nowTs : 300;
        if (state != 0) leftSec = 0;

        std::string mchName;
        auto mchRow = db.queryOne("SELECT mch_name FROM merchant WHERE id=?", {row["mch_id"]});
        if (!mchRow.empty()) mchName = mchRow["mch_name"];

        bool isWx = row["pay_type"].find("wx") != std::string::npos;
        bool isAli = row["pay_type"].find("ali") != std::string::npos;
        std::string payLabel = isWx ? "\xe5\xbe\xae\xe4\xbf\xa1\xe6\x94\xaf\xe4\xbb\x98" :
                               isAli ? "\xe6\x94\xaf\xe4\xbb\x98\xe5\xae\x9d\xe6\x94\xaf\xe4\xbb\x98" :
                               "QQ\xe6\x94\xaf\xe4\xbb\x98";

        std::string storedPayUrl = row.count("pay_url") ? row["pay_url"] : "";
        std::string storedQrcode = row.count("qrcode") ? row["qrcode"] : "";
        
        // 如果 payUrl 是多重编码的 Alipay Scheme，进行一次解码
        // 检查是否是 mdeduct-landing 反风控 URL（多重编码）
        if (!storedPayUrl.empty() && storedPayUrl.find("mdeduct-landing") != std::string::npos) {
            // 这是反风控 URL，可能被多重编码了，尝试解码一次
            std::string decoded = urlDecode(storedPayUrl);
            if (!decoded.empty() && decoded != storedPayUrl) {
                storedPayUrl = decoded;
            }
        }

        // [POST] 前缀：上游接口要求 POST 表单提交（如彩虹易 submit.php）
        bool isPostForm = storedPayUrl.rfind("[POST]", 0) == 0;
        std::string postUrl, postBody;
        if (isPostForm) {
            std::string raw = storedPayUrl.substr(6);
            auto qPos = raw.find('?');
            if (qPos != std::string::npos) {
                postUrl  = raw.substr(0, qPos);
                postBody = raw.substr(qPos + 1);
            } else {
                postUrl = raw;
            }
            storedPayUrl = raw; // 普通字段也返回去掉标记的 URL
        }
        bool isRedirect = !isPostForm &&
                          !storedPayUrl.empty() &&
                          storedPayUrl.substr(0, 4) == "http" &&
                          storedQrcode.empty();

        Json::Value data;
        data["orderId"] = orderId;
        data["amount"]  = row["amount"];
        data["subject"] = row["subject"];
        data["payType"] = row["pay_type"];
        data["payLabel"] = payLabel;
        data["mchName"] = mchName;
        data["siteName"] = siteName;
        data["state"]   = state;
        data["leftSec"] = (Json::Int64)leftSec;
        data["returnUrl"] = row.count("return_url") ? row["return_url"] : "";
        data["payUrl"]  = storedPayUrl;
        data["redirect"] = isRedirect;
        data["postForm"] = isPostForm;
        data["postUrl"]  = postUrl;
        data["postBody"] = postBody;
        data["templateId"] = PayDb::instance().getSetting("templateId", "default");

        // 手动确认免签: 解析 ext_json 暴露字段给收银台
        std::string extStr = row.count("ext_json") ? row["ext_json"] : "";
        if (!extStr.empty()) {
            Json::Value ext;
            Json::CharReaderBuilder erb;
            std::istringstream ess(extStr);
            std::string eerrs;
            if (Json::parseFromStream(erb, ess, &ext, &eerrs)) {
                if (ext.get("need_confirm", false).asBool()) {
                    data["needConfirm"] = true;
                    data["realAmount"] = ext.get("receipt_amount",
                        row.count("real_amount") ? row["real_amount"] : row["amount"]).asString();
                    data["confirmUrl"] = "/gateway/confirm/" + orderId;
                    data["pendingReview"] = ext.get("pending_review", false).asBool();
                }
                std::string qrImg = ext.get("qrcode_image", "").asString();
                if (!qrImg.empty()) data["qrcodeImage"] = qrImg;
            }
        }

        Json::Value j; j["code"] = 0; j["data"] = data;
        cb(drogon::HttpResponse::newHttpJsonResponse(j));
    }

    // ══════════════════════════════════════════════════════════
    //  易支付兼容接口 (保持向后兼容)
    // ══════════════════════════════════════════════════════════
    void submitGet(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        handleEpaySubmit(req, std::move(cb));
    }
    void submitPost(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        handleEpaySubmit(req, std::move(cb));
    }

    void mapi(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto params = EpaySign::paramsFromRequest(req);
        std::string err, orderId;
        if (!createEpayOrder(params, err, orderId)) {
            Json::Value r; r["code"] = 0; r["msg"] = err;
            cb(drogon::HttpResponse::newHttpJsonResponse(r)); return;
        }
        Json::Value r;
        r["code"] = 1; r["msg"] = "订单创建成功";
        r["trade_no"] = orderId;
        r["payurl"] = "/gateway/cashier/" + orderId;
        cb(drogon::HttpResponse::newHttpJsonResponse(r));
    }

    // GET /api.php?act=...
    void apiQuery(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        handleEpayApi(req, std::move(cb));
    }
    // POST /api.php
    void apiQueryPost(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        handleEpayApi(req, std::move(cb));
    }

    // ══════════════════════════════════════════════════════════
    //  微信点金计划 iframe 页 GET /gold.php?out_trade_no=xxx
    // ══════════════════════════════════════════════════════════
    void goldPage(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto &db = PayDb::instance();
        std::string outTradeNo = req->getParameter("out_trade_no");
        std::string siteUrl = db.getSetting("siteUrl", "");

        std::string errMsg;
        std::string amount;
        std::string jumpUrl;

        if (outTradeNo.empty()) {
            errMsg = "\xe8\xae\xa2\xe5\x8d\x95\xe5\x8f\xb7\xe4\xb8\x8d\xe8\x83\xbd\xe4\xb8\xba\xe7\xa9\xba"; // 订单号不能为空
        } else {
            auto order = db.queryOne("SELECT * FROM pay_order WHERE order_id=?", {outTradeNo});
            if (order.empty())
                order = db.queryOne("SELECT * FROM pay_order WHERE mch_order_no=?", {outTradeNo});
            if (order.empty()) {
                errMsg = "\xe8\xae\xa2\xe5\x8d\x95\xe5\x8f\xb7\xe4\xb8\x8d\xe5\xad\x98\xe5\x9c\xa8"; // 订单号不存在
            } else {
                amount = order.count("amount") ? order["amount"] : "0";
                jumpUrl = order.count("return_url") ? order["return_url"] : "";
                if (jumpUrl.empty()) jumpUrl = siteUrl;
            }
        }

        std::ostringstream html;
        html << R"(<!DOCTYPE html><html lang="zh"><head><meta charset="UTF-8">)"
             << R"(<title>)" << "\xe6\x94\xaf\xe4\xbb\x98\xe7\xbb\x93\xe6\x9e\x9c" << R"(</title>)"
             << R"(<meta name="viewport" content="width=device-width,initial-scale=1.0,maximum-scale=1.0,user-scalable=0">)"
             << R"(<style>*{margin:0;padding:0}body{background:#f5f5f5;font-family:-apple-system,sans-serif})"
             << R"(.main{width:100%;display:flex;justify-content:center}.container{display:flex;flex-direction:column;align-items:center;margin-top:40px})"
             << R"(.icon-r{font-size:64px;margin-bottom:12px}.text{font-size:24px;color:#333;margin:12px 0}.msg{font-size:14px;color:#666;text-align:center})"
             << R"(.btn a{display:inline-block;margin-top:24px;padding:10px 24px;background:#07c160;color:#fff;border-radius:4px;text-decoration:none;font-size:14px})"
             << R"(</style>)"
             << R"(<script src="https://wx.gtimg.com/pay_h5/goldplan/js/jgoldplan-1.0.0.js"></script>)"
             << R"(</head><body><div class="main"><div class="container">)";

        if (!errMsg.empty()) {
            html << R"(<div class="icon-r">&#x274C;</div>)"
                 << R"(<div class="text">)" << "\xe9\x94\x99\xe8\xaf\xaf\xe6\x8f\x90\xe7\xa4\xba" << R"(</div>)"
                 << R"(<div class="msg"><p>)" << errMsg << R"(</p></div>)"
                 << R"(</div></div>)"
                 << R"(<script>)"
                 << R"(parent.postMessage(JSON.stringify({action:'onIframeReady',displayStyle:'SHOW_CUSTOM_PAGE'}),'https://payapp.weixin.qq.com');)"
                 << R"(</script>)";
        } else {
            html << R"(<div class="icon-r">&#x2705;</div>)"
                 << R"(<div class="text">&yen;)" << amount << R"(</div>)"
                 << R"(<div class="msg"><p>)" << "\xe6\x94\xaf\xe4\xbb\x98\xe6\x88\x90\xe5\x8a\x9f\xef\xbc\x81\xe8\xaf\xb7\xe7\x82\xb9\xe5\x87\xbb\xe6\x8c\x89\xe9\x92\xae\xe8\xbf\x94\xe5\x9b\x9e\xe5\x95\x86\xe5\xae\xb6\xe9\xa1\xb5\xe9\x9d\xa2" << R"(</p></div>)"
                 << R"HTML(<div class="btn"><a href="javascript:JumpOut()">)HTML" << "\xe8\xbf\x94\xe5\x9b\x9e\xe5\x95\x86\xe5\xae\xb6" << R"HTML(</a></div>)HTML"
                 << R"(</div></div>)"
                 << R"(<script>)"
                 << R"(parent.postMessage(JSON.stringify({action:'onIframeReady',displayStyle:'SHOW_CUSTOM_PAGE'}),'https://payapp.weixin.qq.com');)"
                 << R"(function JumpOut(){parent.postMessage(JSON.stringify({action:'jumpOut',jumpOutUrl:')" << jumpUrl << R"('}),'https://payapp.weixin.qq.com');})"
                 << R"(</script>)";
        }
        html << R"(</body></html>)";

        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setContentTypeCode(drogon::CT_TEXT_HTML);
        resp->setBody(html.str());
        cb(resp);
    }

private:
    static void gatewayResp(std::function<void(const drogon::HttpResponsePtr &)> &cb,
                            int code, const std::string &msg) {
        Json::Value r; r["code"] = code; r["msg"] = msg;
        auto resp = drogon::HttpResponse::newHttpJsonResponse(r);
        // 添加 CORS 头支持跨域请求
        resp->addHeader("Access-Control-Allow-Origin", "*");
        resp->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        resp->addHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
        cb(resp);
    }

    static PayDb::Row findMch(PayDb &db, const std::string &idOrNo) {
        if (idOrNo.empty()) return {};
        auto mch = db.queryOne("SELECT * FROM merchant WHERE id=? AND state=1", {idOrNo});
        if (mch.empty())
            mch = db.queryOne("SELECT * FROM merchant WHERE mch_no=? AND state=1", {idOrNo});
        return mch;
    }

    static std::string requestBaseUrl(const drogon::HttpRequestPtr &req) {
        // 1. DB 系统设置 site_url（管理后台可改）
        auto &db = PayDb::instance();
        std::string siteUrl = db.getSetting("site_url", "");
        if (siteUrl.empty()) siteUrl = db.getSetting("siteUrl", "");
        while (!siteUrl.empty() && siteUrl.back() == '/') siteUrl.pop_back();
        if (!siteUrl.empty()) return siteUrl;
        // 2. config.json wepay.notify_base_url
        try {
            auto &cfg = drogon::app().getCustomConfig();
            if (cfg.isMember("wepay")) {
                std::string nbUrl = cfg["wepay"].get("notify_base_url", "").asString();
                while (!nbUrl.empty() && nbUrl.back() == '/') nbUrl.pop_back();
                if (!nbUrl.empty()) return nbUrl;
            }
        } catch (...) {}
        // 3. fallback: 从请求头推断（内网地址，外部上游可能无法访问）
        std::string host = req->getHeader("host");
        if (host.empty()) host = "127.0.0.1";
        std::string proto = req->getHeader("x-forwarded-proto");
        if (proto.empty()) proto = "http";
        return proto + "://" + host;
    }

    void handleEpaySubmit(const drogon::HttpRequestPtr &req,
                          std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto params = EpaySign::paramsFromRequest(req);
        std::string err, orderId;
        if (!createEpayOrder(params, err, orderId)) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k400BadRequest);
            resp->setBody(err); cb(resp); return;
        }
        auto resp = drogon::HttpResponse::newRedirectionResponse("/gateway/cashier/" + orderId);
        cb(resp);
    }

    // ══════════════════════════════════════════════════════════
    //  彩虹易支付兼容 API 分发 (api.php?act=query|order|orders|settle|refund)
    // ══════════════════════════════════════════════════════════
    void handleEpayApi(const drogon::HttpRequestPtr &req,
                       std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto params = EpaySign::paramsFromRequest(req);
        auto get = [&](const std::string &k) -> std::string {
            auto it = params.find(k); return it == params.end() ? "" : it->second;
        };
        std::string act = get("act");
        auto &db = PayDb::instance();

        auto jsonResp = [&](const Json::Value &r) {
            cb(drogon::HttpResponse::newHttpJsonResponse(r));
        };
        auto errResp = [&](int code, const std::string &msg) {
            Json::Value r; r["code"] = code; r["msg"] = msg; jsonResp(r);
        };

        // ── act=query: 商户信息 ──
        if (act == "query") {
            std::string pid = get("pid");
            std::string key = get("key");
            auto mch = findMch(db, pid);
            if (mch.empty()) { errResp(-3, "商户ID不存在"); return; }
            if (key != mch["mch_key"]) { errResp(-3, "商户密钥错误"); return; }

            std::string mchId = mch["id"];
            auto cntAll = db.queryOne("SELECT COUNT(*) AS c FROM pay_order WHERE mch_id=?", {mchId});
            std::string today = []{ char b[12]; time_t t=std::time(nullptr); struct tm tm;
#ifdef _WIN32
                localtime_s(&tm, &t);
#else
                localtime_r(&t, &tm);
#endif
                std::strftime(b, sizeof(b), "%Y-%m-%d", &tm); return std::string(b); }();
            std::string lastday = [&]{ char b[12]; time_t t=std::time(nullptr)-86400; struct tm tm;
#ifdef _WIN32
                localtime_s(&tm, &t);
#else
                localtime_r(&t, &tm);
#endif
                std::strftime(b, sizeof(b), "%Y-%m-%d", &tm); return std::string(b); }();

            auto cntToday = db.queryOne(
                "SELECT COUNT(*) AS c FROM pay_order WHERE mch_id=? AND state=1 AND DATE(created_at)=?",
                {mchId, today});
            auto cntLast = db.queryOne(
                "SELECT COUNT(*) AS c FROM pay_order WHERE mch_id=? AND state=1 AND DATE(created_at)=?",
                {mchId, lastday});

            Json::Value r;
            r["code"] = 1;
            r["pid"] = std::stoi(mchId);
            r["key"] = mch["mch_key"];
            r["active"] = std::stoi(mch["state"]);
            r["money"] = mch.count("balance") ? mch["balance"] : "0.00";
            r["account"] = mch.count("settle_account") ? mch["settle_account"] : "";
            r["username"] = mch.count("settle_name") ? mch["settle_name"] : "";
            r["orders"] = cntAll.empty() ? 0 : std::stoi(cntAll["c"]);
            r["orders_today"] = cntToday.empty() ? 0 : std::stoi(cntToday["c"]);
            r["orders_lastday"] = cntLast.empty() ? 0 : std::stoi(cntLast["c"]);
            jsonResp(r);
        }
        // ── act=order: 查询单个订单 ──
        else if (act == "order") {
            std::string pid = get("pid");
            std::string key = get("key");
            std::string tradeNo    = get("trade_no");
            std::string outTradeNo = get("out_trade_no");
            std::string signStr    = get("sign");

            // 支持系统内部签名查询（trade_no + sign）
            if (!signStr.empty() && !tradeNo.empty()) {
                std::string sysKey = db.getSetting("sys_key", "");
                std::string expect = Md5Utils::md5(sysKey + tradeNo + sysKey);
                if (expect == signStr) {
                    auto order = db.queryOne("SELECT * FROM pay_order WHERE order_id=?", {tradeNo});
                    if (order.empty()) { errResp(-1, "订单号不存在"); return; }
                    buildOrderResp(order, jsonResp); return;
                }
            }

            auto mch = findMch(db, pid);
            if (mch.empty()) { errResp(-3, "商户ID不存在"); return; }
            if (key != mch["mch_key"]) { errResp(-3, "商户密钥错误"); return; }

            PayDb::Row order;
            if (!tradeNo.empty())
                order = db.queryOne("SELECT * FROM pay_order WHERE order_id=? AND mch_id=?",
                                    {tradeNo, mch["id"]});
            else if (!outTradeNo.empty())
                order = db.queryOne("SELECT * FROM pay_order WHERE mch_order_no=? AND mch_id=?",
                                    {outTradeNo, mch["id"]});
            else { errResp(-4, "订单号不能为空"); return; }
            if (order.empty()) { errResp(-1, "订单号不存在"); return; }
            buildOrderResp(order, jsonResp);
        }
        // ── act=orders: 订单列表 ──
        else if (act == "orders") {
            std::string pid = get("pid");
            std::string key = get("key");
            auto mch = findMch(db, pid);
            if (mch.empty()) { errResp(-3, "商户ID不存在"); return; }
            if (key != mch["mch_key"]) { errResp(-3, "商户密钥错误"); return; }

            int limit = 10, offset = 0;
            try { limit = std::stoi(get("limit")); } catch (...) {}
            try { offset = std::stoi(get("offset")); } catch (...) {}
            if (limit > 50) limit = 50;
            if (limit < 1)  limit = 10;

            std::string sql = "SELECT o.*, pt.name AS typename FROM pay_order o "
                              "LEFT JOIN pay_type pt ON o.pay_type=pt.code "
                              "WHERE o.mch_id=?";
            std::vector<std::string> sqlParams = {mch["id"]};

            std::string statusStr = get("status");
            if (!statusStr.empty()) {
                sql += " AND o.state=?";
                sqlParams.push_back(statusStr);
            }
            sql += " ORDER BY o.created_at DESC LIMIT " + std::to_string(limit)
                   + " OFFSET " + std::to_string(offset);

            auto rows = db.query(sql, sqlParams);
            Json::Value data(Json::arrayValue);
            for (auto &row : rows) {
                Json::Value item;
                item["trade_no"]     = row.count("order_id") ? row.at("order_id") : "";
                item["out_trade_no"] = row.count("mch_order_no") ? row.at("mch_order_no") : "";
                item["type"]         = row.count("typename") ? row.at("typename") : row.at("pay_type");
                item["pid"]          = std::stoi(mch["id"]);
                item["addtime"]      = row.count("created_at") ? row.at("created_at") : "";
                item["endtime"]      = row.count("pay_time") ? row.at("pay_time") : "";
                item["name"]         = row.count("subject") ? row.at("subject") : "";
                item["money"]        = row.count("amount") ? row.at("amount") : "0";
                item["param"]        = row.count("param") ? row.at("param") : "";
                item["status"]       = std::stoi(row.count("state") ? row.at("state") : "0");
                data.append(item);
            }
            Json::Value r;
            r["code"] = 1; r["msg"] = "查询订单记录成功！";
            r["count"] = (int)data.size(); r["data"] = data;
            jsonResp(r);
        }
        // ── act=settle: 查询结算记录 ──
        else if (act == "settle") {
            std::string pid = get("pid");
            std::string key = get("key");
            auto mch = findMch(db, pid);
            if (mch.empty()) { errResp(-3, "商户ID不存在"); return; }
            if (key != mch["mch_key"]) { errResp(-3, "商户密钥错误"); return; }

            int limit = 10, offset = 0;
            try { limit = std::stoi(get("limit")); } catch (...) {}
            try { offset = std::stoi(get("offset")); } catch (...) {}
            if (limit > 50) limit = 50;

            auto rows = db.query(
                "SELECT * FROM settle_order WHERE mch_id=? ORDER BY created_at DESC "
                "LIMIT " + std::to_string(limit) + " OFFSET " + std::to_string(offset),
                {mch["id"]});

            Json::Value data(Json::arrayValue);
            for (auto &row : rows) {
                Json::Value item;
                item["id"]        = row.count("id") ? row.at("id") : "";
                item["money"]     = row.count("amount") ? row.at("amount") : "0";
                item["realmoney"] = row.count("actual_amount") ? row.at("actual_amount") : "0";
                item["addtime"]   = row.count("created_at") ? row.at("created_at") : "";
                item["endtime"]   = row.count("completed_at") ? row.at("completed_at") : "";
                item["status"]    = std::stoi(row.count("state") ? row.at("state") : "0");
                item["account"]   = row.count("account") ? row.at("account") : "";
                item["username"]  = row.count("account_name") ? row.at("account_name") : "";
                data.append(item);
            }
            Json::Value r;
            r["code"] = 1; r["msg"] = "查询结算记录成功！"; r["data"] = data;
            jsonResp(r);
        }
        // ── act=refund: 商户发起退款 ──
        else if (act == "refund") {
            std::string pid = get("pid");
            std::string key = get("key");
            auto mch = findMch(db, pid);
            if (mch.empty()) { errResp(-3, "商户ID不存在"); return; }
            if (key != mch["mch_key"]) { errResp(-3, "商户密钥错误"); return; }

            // 检查商户退款权限
            std::string refundPerm = mch.count("allow_refund") ? mch["allow_refund"] : "1";
            if (refundPerm == "0") { errResp(-2, "商户未开启订单退款API接口"); return; }

            std::string money = get("money");
            double refundAmt = 0;
            try { refundAmt = std::stod(money); } catch (...) {}
            if (refundAmt <= 0) { errResp(-1, "金额输入错误"); return; }

            std::string tradeNo = get("trade_no");
            std::string outTradeNo = get("out_trade_no");
            PayDb::Row order;
            if (!tradeNo.empty())
                order = db.queryOne("SELECT * FROM pay_order WHERE order_id=? AND mch_id=?",
                                    {tradeNo, mch["id"]});
            else if (!outTradeNo.empty())
                order = db.queryOne("SELECT * FROM pay_order WHERE mch_order_no=? AND mch_id=?",
                                    {outTradeNo, mch["id"]});
            if (order.empty()) { errResp(-1, "当前订单不存在！"); return; }
            if (order["state"] != "1" && order["state"] != "2") {
                errResp(-1, "该订单状态不支持退款！"); return;
            }

            double orderAmt = 0;
            try { orderAmt = std::stod(order["amount"]); } catch (...) {}
            double existRefund = 0;
            try { existRefund = std::stod(order.count("refund_amount") ? order["refund_amount"] : "0"); } catch (...) {}
            if (refundAmt > orderAmt - existRefund) {
                errResp(-1, "退款金额超过可退款金额"); return;
            }

            // 检查商户余额
            double mchBalance = 0;
            try { mchBalance = std::stod(mch.count("balance") ? mch["balance"] : "0"); } catch (...) {}
            if (refundAmt > mchBalance) { errResp(-1, "商户余额不足，请先充值"); return; }

            std::string refundNo = ChannelService::generateRefundNo();
            long long now = std::time(nullptr);
            int mchId = std::stoi(mch["id"]);

            db.exec("INSERT INTO refund_order(refund_no,order_id,mch_id,mch_refund_no,"
                    "channel_id,pay_type,pay_amount,refund_amount,reason,state,created_at,updated_at) "
                    "VALUES(?,?,?,?,?,?,?,?,?,0,?,?)",
                    {refundNo, order["order_id"], std::to_string(mchId), "",
                     order["channel_id"], order["pay_type"], order["amount"],
                     ChannelService::fmtAmount(refundAmt), "商户API退款",
                     std::to_string(now), std::to_string(now)});

            db.exec("UPDATE pay_order SET refund_amount=?,state=2,updated_at=? WHERE order_id=?",
                    {ChannelService::fmtAmount(existRefund + refundAmt),
                     std::to_string(now), order["order_id"]});

            ChannelService::changeMchBalance(mchId, 6, refundAmt, "refund", refundNo, "API退款");

            Json::Value r;
            r["code"] = 0; r["msg"] = "退款成功！退款金额¥" + ChannelService::fmtAmount(refundAmt);
            r["refund_no"] = refundNo;
            r["money"] = ChannelService::fmtAmount(refundAmt);
            jsonResp(r);
        }
        else {
            errResp(-5, "No Act!");
        }
    }

    // 构建单个订单的响应JSON (彩虹易格式)
    static void buildOrderResp(const PayDb::Row &order,
                               std::function<void(const Json::Value &)> jsonResp) {
        Json::Value r;
        r["code"] = 1; r["msg"] = "succ";
        r["trade_no"]     = order.count("order_id") ? order.at("order_id") : "";
        r["out_trade_no"] = order.count("mch_order_no") ? order.at("mch_order_no") : "";
        r["type"]         = order.count("pay_type") ? order.at("pay_type") : "";
        r["pid"]          = order.count("mch_id") ? std::stoi(order.at("mch_id")) : 0;
        r["addtime"]      = order.count("created_at") ? order.at("created_at") : "";
        r["endtime"]      = order.count("pay_time") ? order.at("pay_time") : "";
        r["name"]         = order.count("subject") ? order.at("subject") : "";
        r["money"]        = order.count("amount") ? order.at("amount") : "0";
        r["param"]        = order.count("param") ? order.at("param") : "";
        r["status"]       = order.count("state") ? std::stoi(order.at("state")) : 0;
        r["buyer"]        = order.count("buyer") ? order.at("buyer") : "";
        r["payurl"]       = order.count("pay_url") ? order.at("pay_url") : "";
        jsonResp(r);
    }

    bool createEpayOrder(const std::map<std::string, std::string> &params,
                         std::string &err, std::string &orderId) {
        auto get = [&](const std::string &k) -> std::string {
            auto it = params.find(k); return it == params.end() ? "" : it->second;
        };
        auto &db = PayDb::instance();
        std::string pid = get("pid");
        auto mch = findMch(db, pid);
        if (mch.empty()) { err = "商户不存在"; return false; }

        std::string mchKey = mch["mch_key"];
        std::string sign = get("sign");
        if (!EpaySign::verify(params, mchKey, sign)) { err = "签名错误"; return false; }

        std::string type = get("type");
        std::string outTradeNo = get("out_trade_no");
        std::string money = get("money");
        std::string notifyUrl = get("notify_url");
        std::string returnUrl = get("return_url");
        std::string name = get("name");

        if (type.empty() || outTradeNo.empty() || money.empty()) {
            err = "参数不完整"; return false;
        }

        // 类型转换: wxpay/alipay/1/2 -> 统一 payType code
        std::string payType = type;
        if (type == "1" || type == "wechat" || type == "weixin") payType = "wxpay";
        if (type == "2" || type == "zfb") payType = "alipay";

        double amount = 0;
        try { amount = std::stod(money); } catch (...) { err = "金额格式错误"; return false; }
        if (amount <= 0) { err = "金额错误"; return false; }

        int mchId = std::stoi(mch["id"]);
        std::string clientIp = get("clientip");

        // 风控检查
        auto riskResult = RiskControlService::check(mchId, clientIp, notifyUrl, name, amount);
        if (!riskResult.pass) { err = riskResult.reason; return false; }

        auto exist = db.queryOne("SELECT id FROM pay_order WHERE mch_id=? AND mch_order_no=?",
                                 {std::to_string(mchId), outTradeNo});
        if (!exist.empty()) { err = "商户订单号已存在"; return false; }

        auto channel = ChannelService::selectChannel(mchId, payType, amount);

        orderId = ChannelService::generateOrderId(db.getSetting("order_prefix", "W"));
        long long now = std::time(nullptr);
        int closeMin = 5;
        try { closeMin = std::stoi(db.getSetting("close_minutes", "5")); } catch (...) {}

        double mchRate = ChannelService::getMchRate(mchId, channel.channelId);
        double mchFee  = ChannelService::calcFee(amount, mchRate);

        db.exec(
            "INSERT INTO pay_order(order_id,mch_id,mch_order_no,channel_id,pay_type,"
            "amount,real_amount,mch_fee_rate,mch_fee_amount,subject,param,"
            "notify_url,return_url,client_ip,state,expire_time,created_at,updated_at) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,0,?,?,?)",
            {orderId, std::to_string(mchId), outTradeNo,
             std::to_string(channel.channelId), payType,
             ChannelService::fmtAmount(amount), ChannelService::fmtAmount(amount),
             ChannelService::fmtAmount(mchRate), ChannelService::fmtAmount(mchFee),
             name, get("param"), notifyUrl, returnUrl, clientIp,
             std::to_string(now + closeMin * 60),
             std::to_string(now), std::to_string(now)});
        return true;
    }

    // 通过 PHP API 创建订单（调用 PHP 的 alipay_scheme.php）
    void createPhp(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        // 直接转发到 create 方法，但设置标志使用 PHP API
        // 这里复用 create 的逻辑，只是在调用 PHP API 时不降级到 C++ AlipayLib
        
        auto params = EpaySign::paramsFromRequest(req);
        auto get = [&](const std::string &k) -> std::string {
            auto it = params.find(k); return it == params.end() ? "" : it->second;
        };

        std::string mchIdStr   = get("mch_id");
        std::string outTradeNo = get("out_trade_no");
        std::string payType    = get("pay_type");
        std::string amountStr  = get("amount");
        std::string subject    = get("subject");
        std::string notifyUrl  = get("notify_url");
        std::string returnUrl  = get("return_url");
        std::string notifyEmail= get("notify_email");
        std::string param      = get("param");
        std::string sign       = get("sign");
        std::string body       = get("body");

        // 支付宝转账/红包不需要商户验证
        bool isAlipayDirect = (payType == "alipay_scan" || payType == "alipay_transfer" || payType == "alipay_redpacket");
        
        std::unordered_map<std::string, std::string> mch;
        int mchId = 0;
        std::string mchKey = "";
        
        if (isAlipayDirect) {
            mchId = 0;
            mchKey = "";
        } else {
            if (mchIdStr.empty() || sign.empty()) {
                gatewayResp(cb, -1, "参数不完整"); return;
            }
            
            PayDb &db = PayDb::instance();
            bool isNumericId = !mchIdStr.empty() && std::all_of(mchIdStr.begin(), mchIdStr.end(), ::isdigit);
            if (isNumericId) {
                mch = db.queryOne("SELECT * FROM merchant WHERE id=? AND state=1", {mchIdStr});
            }
            if (mch.empty()) {
                mch = db.queryOne("SELECT * FROM merchant WHERE mch_no=? AND state=1", {mchIdStr});
            }
            if (mch.empty()) { gatewayResp(cb, -2, "商户不存在或已禁用"); return; }
            
            mchId = std::stoi(mch["id"]);
            mchKey = mch["mch_key"];
        }

        std::string publicKey = "";
        if (!isAlipayDirect) {
            publicKey = mch.count("public_key") ? mch["public_key"] : "";
        }

        // 验证签名（非支付宝直接转账/红包）
        if (!isAlipayDirect) {
            if (!EpaySign::verify(params, mchKey, sign, publicKey)) {
                gatewayResp(cb, -3, "签名验证失败"); return;
            }
        }

        // 调用 PHP API 生成 Scheme
        std::string alipayUserId = get("alipay_user_id");
        std::string alipayUserName = get("alipay_user_name");
        std::string alipayLoginId = get("alipay_login_id");
        std::string memo = get("memo");
        std::string remark = get("remark");
        
        if (alipayUserId.empty()) {
            gatewayResp(cb, -9, "缺少支付宝用户ID"); return;
        }

        // 生成订单 ID
        PayDb &db = PayDb::instance();
        std::string orderId = ChannelService::generateOrderId(
            db.getSetting("order_prefix", "W"));
        
        // 创建订单到数据库
        int now = std::time(nullptr);
        int expireTime = now + 30 * 60;  // 30分钟过期
        double amount = std::stod(amountStr);
        
        bool ok = db.exec(
            "INSERT INTO pay_order(order_id,mch_id,app_id,mch_order_no,channel_id,"
            "pay_type,amount,real_amount,mch_fee_rate,mch_fee_amount,"
            "channel_fee_rate,channel_fee_amount,subject,body,param,"
            "notify_url,return_url,client_ip,state,notify_state,"
            "expire_time,created_at,updated_at,buyer_id) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,0,0,?,?,?,?)",
            {orderId, std::to_string(mchId), "", outTradeNo,
             "0", payType,
             ChannelService::fmtAmount(amount), ChannelService::fmtAmount(amount),
             "0", "0",
             "0", "0",
             subject, body, param, notifyUrl, returnUrl, "",
             std::to_string(expireTime), std::to_string(now), std::to_string(now), ""});
        
        if (!ok) {
            gatewayResp(cb, -4, "创建订单失败"); return;
        }

        // 调用 PHP API
        Json::Value phpRequest;
        phpRequest["pay_type"] = payType;
        phpRequest["amount"] = amountStr;
        phpRequest["out_trade_no"] = outTradeNo;
        phpRequest["subject"] = subject;
        phpRequest["cpp_order_id"] = orderId;
        // 传递 C++ 网关的回调 URL，支付宝会回调到 C++ 网关，然后反向代理到 PHP
        phpRequest["notify_url"] = "http://localhost:8088/alipay/notify";
        
        if (payType == "alipay_transfer") {
            phpRequest["alipay_user_id"] = alipayUserId;
            if (!memo.empty()) {
                phpRequest["memo"] = memo;
            }
        } else if (payType == "alipay_redpacket") {
            phpRequest["alipay_login_id"] = alipayLoginId;
            phpRequest["alipay_user_id"] = alipayUserId;
            if (!alipayUserName.empty()) {
                phpRequest["alipay_user_name"] = alipayUserName;
            }
            if (!remark.empty()) {
                phpRequest["remark"] = remark;
            }
        }
        
        // 调用 PHP API
        std::string phpUrl = "http://localhost:61111/api/alipay_scheme.php";
        std::string phpBody = phpRequest.toStyledString();
        auto phpResp = SyncHttp::postJson(phpUrl, phpBody);
        
        if (!phpResp.success || phpResp.status != 200) {
            db.exec("UPDATE pay_order SET state=-1,updated_at=? WHERE order_id=?",
                    {std::to_string(std::time(nullptr)), orderId});
            gatewayResp(cb, -9, "调用 PHP API 失败"); return;
        }
        
        // 解析 PHP 返回的结果
        Json::Value phpResult;
        Json::CharReaderBuilder builder;
        std::string errs;
        std::istringstream phpBodyStream(phpResp.body);
        if (!Json::parseFromStream(builder, phpBodyStream, &phpResult, &errs)) {
            db.exec("UPDATE pay_order SET state=-1,updated_at=? WHERE order_id=?",
                    {std::to_string(std::time(nullptr)), orderId});
            gatewayResp(cb, -9, "解析 PHP 响应失败"); return;
        }
        
        if (phpResult.get("code", 0).asInt() != 1) {
            db.exec("UPDATE pay_order SET state=-1,updated_at=? WHERE order_id=?",
                    {std::to_string(std::time(nullptr)), orderId});
            gatewayResp(cb, -9, "PHP API 返回错误: " + phpResult.get("msg", "未知错误").asString()); return;
        }
        
        // 获取 Scheme 和 PHP 订单 ID
        std::string scheme = phpResult["data"].get("scheme", "").asString();
        std::string phpOrderId = phpResult["data"].get("php_order_id", "").asString();
        
        if (scheme.empty()) {
            db.exec("UPDATE pay_order SET state=-1,updated_at=? WHERE order_id=?",
                    {std::to_string(std::time(nullptr)), orderId});
            gatewayResp(cb, -9, "生成支付宝Scheme失败"); return;
        }
        
        // 保存 mch_order_no 为 PHP 订单号，方便后续查询 PHP 数据库
        std::string qrcodeUrl = "";
        
        // 保存 Scheme、二维码 和 PHP 订单 ID 到 channel_data
        Json::Value channelData;
        channelData["scheme"] = scheme;
        channelData["type"] = payType;
        channelData["php_order_id"] = phpOrderId;
        channelData["alipay_user_id"] = alipayUserId;
        if (!alipayUserName.empty()) channelData["alipay_user_name"] = alipayUserName;
        if (!memo.empty()) channelData["memo"] = memo;
        if (!remark.empty()) channelData["remark"] = remark;
        
        // 保存 Scheme 和 channel_data（二维码 URL 保存到 channel_data 中）
        if (!qrcodeUrl.empty()) {
            channelData["qrcode_url"] = qrcodeUrl;
        }
        // 保存 mch_order_no 为 PHP 订单的 out_trade_no，方便后续查询 PHP 数据库
        db.exec("UPDATE pay_order SET mch_order_no=?,pay_url=?,channel_data=?,updated_at=? WHERE order_id=?",
                {outTradeNo, scheme, channelData.toStyledString(), std::to_string(std::time(nullptr)), orderId});
        
        // 返回支付信息
        // C++ 收银台显示，但支付链接指向 PHP（PHP 有完整的回调处理）
        Json::Value result;
        result["code"] = 1;
        result["msg"] = "订单创建成功";
        result["data"]["order_id"] = orderId;
        result["data"]["scheme"] = scheme;
        result["data"]["amount"] = amountStr;
        // 支付链接指向 PHP 的 submit.php（PHP 会处理支付宝回调）
        result["data"]["pay_url"] = "http://localhost:61111/submit.php?pid=1001288771895253&type=alipay&out_trade_no=" + 
                                   outTradeNo + "&name=" + urlEncode(subject) + "&money=" + amountStr;
        result["data"]["qr_url"] = "https://chart.googleapis.com/chart?chs=300x300&chld=L|0&cht=qr&chl=" + 
                                   urlEncode(scheme);
        
        auto resp = drogon::HttpResponse::newHttpJsonResponse(result);
        resp->setStatusCode(drogon::k200OK);
        cb(resp);
    }

    // 同步订单状态（PHP 调用此接口来更新 C++ 的订单状态）
    void syncStatus(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto json = req->getJsonObject();
        if (!json) {
            gatewayResp(cb, -1, "请求格式错误"); return;
        }

        std::string cppOrderId = (*json).get("cpp_order_id", "").asString();
        std::string phpOrderId = (*json).get("php_order_id", "").asString();
        int status = (*json).get("status", 0).asInt();
        
        if (cppOrderId.empty()) {
            gatewayResp(cb, -1, "缺少参数：cpp_order_id"); return;
        }

        // 验证签名（可选，增加安全性）
        std::string sign = (*json).get("sign", "").asString();
        // 这里可以添加签名验证逻辑
        
        // 更新订单状态
        PayDb &db = PayDb::instance();
        int now = std::time(nullptr);
        
        // 状态映射：1=已支付
        int cppStatus = (status == 1) ? 1 : 0;
        
        bool ok = db.exec(
            "UPDATE pay_order SET state=?,updated_at=? WHERE order_id=?",
            {std::to_string(cppStatus), std::to_string(now), cppOrderId}
        );
        
        if (!ok) {
            gatewayResp(cb, -1, "更新订单失败"); return;
        }
        
        // 如果订单已支付，触发商户回调（带 C++ 签名）
        if (status == 1) {
            auto order = db.queryOne(
                "SELECT * FROM pay_order WHERE order_id=?", {cppOrderId});
            if (!order.empty()) {
                std::string notifyUrl = order.count("notify_url") ? order["notify_url"] : "";
                if (!notifyUrl.empty()) {
                    std::string mid = order.count("mch_id") ? order["mch_id"] : "0";
                    auto mch = db.queryOne("SELECT mch_key FROM merchant WHERE id=?", {mid});
                    std::string mchKey = mch.empty() ? "" : mch["mch_key"];

                    std::map<std::string, std::string> m;
                    m["trade_no"] = cppOrderId;
                    m["out_trade_no"] = order.count("mch_order_no") ? order["mch_order_no"] : "";
                    m["type"] = order.count("pay_type") ? order["pay_type"] : "";
                    m["money"] = order.count("amount") ? order["amount"] : "";
                    m["trade_status"] = "TRADE_SUCCESS";
                    m["sign"] = EpaySign::sign(m, mchKey);
                    m["sign_type"] = "MD5";

                    std::string sep = (notifyUrl.find('?') == std::string::npos) ? "?" : "&";
                    std::string query;
                    for (auto &[k, v] : m) {
                        if (!query.empty()) query += "&";
                        query += k + "=" + urlEncode(v);
                    }
                    HttpCaller::asyncGet(notifyUrl + sep + query);
                }
            }
        }
        
        gatewayResp(cb, 1, "同步成功");
    }

    // 读取 PHP 生成的二维码文件
    void getQrcode(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                   std::string filename) {
        // 防止路径遍历攻击
        if (filename.find("..") != std::string::npos || filename.find("/") != std::string::npos) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k404NotFound);
            cb(resp);
            return;
        }
        
        // 构建文件路径（PHP 生成的二维码目录）
        std::string phpPayDir = drogon::app().getDocumentRoot() + "/../php-pay/qrcodes/";
        std::string filePath = phpPayDir + filename;
        
        // 检查文件是否存在
        if (!std::filesystem::exists(filePath)) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k404NotFound);
            cb(resp);
            return;
        }
        
        // 读取文件
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k500InternalServerError);
            cb(resp);
            return;
        }
        
        std::string fileContent((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
        file.close();
        
        // 返回图片
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setContentTypeCode(drogon::CT_IMAGE_PNG);
        resp->setBody(fileContent);
        resp->setStatusCode(drogon::k200OK);
        cb(resp);
    }
    
    // URL 编码辅助函数
    static std::string urlEncode(const std::string& str) {
        std::string result;
        for (char c : str) {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                result += c;
            } else {
                result += '%';
                result += "0123456789ABCDEF"[c >> 4];
                result += "0123456789ABCDEF"[c & 15];
            }
        }
        return result;
    }
    
    // URL 解码辅助函数
    static std::string urlDecode(const std::string& str) {
        std::string result;
        for (size_t i = 0; i < str.length(); ++i) {
            if (str[i] == '%' && i + 2 < str.length()) {
                std::string hex = str.substr(i + 1, 2);
                char c = (char)std::stoi(hex, nullptr, 16);
                result += c;
                i += 2;
            } else if (str[i] == '+') {
                result += ' ';
            } else {
                result += str[i];
            }
        }
        return result;
    }
    
    // 手动触发订单支付成功（用于本地测试，模拟支付宝回调）
    void manualNotify(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto orderId = req->getParameter("order_id");
        
        if (orderId.empty()) {
            gatewayResp(cb, -1, "缺少 order_id 参数");
            return;
        }
        
        auto &db = PayDb::instance();
        long long now = std::time(nullptr);
        
        // 更新订单状态为已支付
        db.exec("UPDATE pay_order SET state=1, pay_time=?, updated_at=? WHERE order_id=?",
                {std::to_string(now), std::to_string(now), orderId});
        
        gatewayResp(cb, 0, "订单已标记为已支付（order_id=" + orderId + "）");
    }
    
    // 反向代理 PHP notify.php（支付宝回调）
    void proxyNotify(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        // 转发到 PHP 的 notify.php
        std::string phpUrl = "http://localhost:61111/notify.php";
        
        // 获取请求体（支付宝回调是 form-urlencoded 格式）
        std::string_view bodyView = req->getBody();
        std::string body(bodyView.begin(), bodyView.end());
        
        // 转发 POST 请求到 PHP（form-urlencoded 格式）
        auto phpResp = SyncHttp::postForm(phpUrl, body);
        
        if (!phpResp.success) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k500InternalServerError);
            resp->setBody("PHP service unavailable");
            cb(resp);
            return;
        }
        
        // 返回 PHP 的响应
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::HttpStatusCode(phpResp.status));
        resp->setBody(phpResp.body);
        resp->setContentTypeCode(drogon::CT_TEXT_PLAIN);
        
        cb(resp);
    }
    
};
