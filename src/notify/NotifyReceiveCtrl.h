// WePay-Cpp — 第三方异步回调接收控制器
// POST /notify/channel/{channelCode}    接收上游支付通道的异步通知
// GET  /notify/channel/{channelCode}    同上(部分通道用GET)
// POST /notify/refund/{channelCode}     接收上游退款通知
// POST /notify/transfer/{channelCode}   接收上游转账通知
// POST /notify/division/{channelCode}   接收上游分账通知
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // 时间库
#include <vector> // 向量容器
#include <map> // 映射容器
#include <set> // 集合容器
#include <json/json.h> // JSON 库
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/ChannelService.h" // 通道服务
#include "../common/EpaySign.h" // 易支付签名工具
#include "../common/Md5Utils.h" // MD5 和签名工具
#include "../common/NotifyTaskService.h" // 通知任务服务
#include "../common/WsBus.h" // WebSocket 消息总线
#include "../common/PasswordUtils.h" // 密码工具

// 第三方异步回调接收控制器类
class NotifyReceiveCtrl : public drogon::HttpController<NotifyReceiveCtrl> {
public:
    METHOD_LIST_BEGIN // 路由列表开始
        ADD_METHOD_TO(NotifyReceiveCtrl::channelNotifyPost,
                      "/notify/channel/{1}", drogon::Post); // 支付通知路由（POST）
        ADD_METHOD_TO(NotifyReceiveCtrl::channelNotifyGet,
                      "/notify/channel/{1}", drogon::Get); // 支付通知路由（GET）
        ADD_METHOD_TO(NotifyReceiveCtrl::refundNotify,
                      "/notify/refund/{1}",  drogon::Post); // 退款通知路由
        ADD_METHOD_TO(NotifyReceiveCtrl::transferNotify,
                      "/notify/transfer/{1}", drogon::Post); // 转账通知路由
        ADD_METHOD_TO(NotifyReceiveCtrl::divisionNotify,
                      "/notify/division/{1}", drogon::Post); // 分账通知路由
    METHOD_LIST_END // 路由列表结束

    // ═══════════════════════════════════════════════════════════════
    // POST/GET /notify/channel/{channelCode} — 支付成功回调
    // ═══════════════════════════════════════════════════════════════
    void channelNotifyPost(const drogon::HttpRequestPtr &req,
                           std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                           std::string channelCode) {
        handleChannelNotify(req, std::move(cb), channelCode); // 处理支付通知
    }

    // GET 方式支付通知（部分通道使用 GET）
    void channelNotifyGet(const drogon::HttpRequestPtr &req,
                          std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                          std::string channelCode) {
        handleChannelNotify(req, std::move(cb), channelCode); // 处理支付通知
    }

    // ═══════════════════════════════════════════════════════════════
    // POST /notify/refund/{channelCode} — 退款回调
    // ═══════════════════════════════════════════════════════════════
    void refundNotify(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                      std::string channelCode) {
        auto &db = PayDb::instance(); // 获取数据库实例
        // 从请求中提取退款单号(不同通道格式不同,这里做通用解析)
        auto params = EpaySign::paramsFromRequest(req); // 解析请求参数
        std::string refundNo = extractField(params, {"refund_no", "out_refund_no", "refundNo"}); // 提取退款号
        std::string channelRefundNo = extractField(params, {"trade_no", "refund_id", "channel_refund_no"}); // 提取通道退款号

        if (refundNo.empty()) { // 检查退款号
            textResp(cb, "fail: missing refund_no"); return; // 退款号为空
        }

        auto refund = db.queryOne("SELECT * FROM refund_order WHERE refund_no=?", {refundNo}); // 查询退款单
        if (refund.empty()) { textResp(cb, "fail: refund not found"); return; } // 退款单不存在

        // 幂等检查：已成功直接返回
        if (refund["state"] == "1") { textResp(cb, "success"); return; } // 已处理则返回成功

        long long now = std::time(nullptr); // 获取当前时间
        db.exec("UPDATE refund_order SET state=1,channel_refund_no=?,updated_at=? WHERE refund_no=?",
                {channelRefundNo, std::to_string(now), refundNo}); // 更新退款状态

        // 退还手续费到商户余额
        double refundFee = 0; // 初始化退款手续费
        try { refundFee = std::stod(refund["refund_fee"]); } catch (...) {} // 解析手续费
        if (refundFee > 0) { // 如果有手续费
            int mchId = std::stoi(refund["mch_id"]); // 获取商户 ID
            ChannelService::changeMchBalance(mchId, 1, refundFee, "refund_fee", refundNo, "退款返还手续费"); // 退还手续费
        }

        textResp(cb, "success"); // 返回成功
    }

    // ═══════════════════════════════════════════════════════════════
    // POST /notify/transfer/{channelCode} — 转账回调
    // ═══════════════════════════════════════════════════════════════
    void transferNotify(const drogon::HttpRequestPtr &req,
                        std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                        std::string channelCode) {
        auto &db = PayDb::instance(); // 获取数据库实例
        auto params = EpaySign::paramsFromRequest(req); // 解析请求参数
        std::string transferNo = extractField(params, {"transfer_no", "out_biz_no", "transferNo"}); // 提取转账号
        std::string channelTransferNo = extractField(params, {"trade_no", "order_id"}); // 提取通道转账号
        std::string status = extractField(params, {"status", "trade_status"}); // 提取状态

        if (transferNo.empty()) { textResp(cb, "fail"); return; } // 转账号为空

        auto t = db.queryOne("SELECT id,mch_id,amount,state FROM transfer_order WHERE transfer_no=?", {transferNo}); // 查询转账单
        if (t.empty()) { textResp(cb, "fail"); return; } // 转账单不存在

        // 已处理
        if (t["state"] != "0") { textResp(cb, "success"); return; } // 已处理则返回成功

        bool ok = (status == "SUCCESS" || status == "00" || status == "TRADE_SUCCESS"); // 判断是否成功
        long long now = std::time(nullptr); // 获取当前时间
        if (ok) { // 如果成功
            db.exec("UPDATE transfer_order SET state=1,channel_transfer_no=?,finished_at=? "
                    "WHERE id=?", {channelTransferNo, std::to_string(now), t["id"]}); // 更新转账状态
        } else { // 如果失败
            // 失败,退还商户余额
            int mchId = std::stoi(t["mch_id"]); // 获取商户 ID
            double amount = 0; try { amount = std::stod(t["amount"]); } catch(...){} // 解析金额
            ChannelService::changeMchBalance(mchId, 1, amount, "transfer_fail",
                transferNo, "转账失败退回"); // 退还金额
            db.exec("UPDATE transfer_order SET state=-1,err_msg='渠道返回失败',finished_at=? WHERE id=?",
                {std::to_string(now), t["id"]}); // 更新转账失败状态
        }
        textResp(cb, "success"); // 返回成功
    }

    // ═══════════════════════════════════════════════════════════════
    // POST /notify/division/{channelCode} — 分账回调
    // ═══════════════════════════════════════════════════════════════
    void divisionNotify(const drogon::HttpRequestPtr &req,
                        std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                        std::string channelCode) {
        auto &db = PayDb::instance(); // 获取数据库实例
        auto params = EpaySign::paramsFromRequest(req); // 解析请求参数
        std::string orderId = extractField(params, {"order_id", "out_trade_no"}); // 提取订单号
        std::string status  = extractField(params, {"status", "trade_status"}); // 提取状态

        if (orderId.empty()) { textResp(cb, "fail"); return; } // 订单号为空

        bool ok = (status == "SUCCESS" || status == "00"); // 判断是否成功
        long long now = std::time(nullptr); // 获取当前时间
        if (ok) { // 如果成功
            db.exec("UPDATE division_record SET state=1,finished_at=? "
                    "WHERE order_id=? AND state=0", {std::to_string(now), orderId}); // 更新分账状态
        } else { // 如果失败
            db.exec("UPDATE division_record SET state=-1,err_msg='渠道返回失败',finished_at=? "
                    "WHERE order_id=? AND state=0", {std::to_string(now), orderId}); // 更新分账失败状态
        }
        textResp(cb, "success"); // 返回成功
    }

private:
    void handleChannelNotify(const drogon::HttpRequestPtr &req,
                             std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                             const std::string &channelCode) {
        auto &db = PayDb::instance();
        auto params = EpaySign::paramsFromRequest(req);

        // 通用提取: 系统订单号 / 上游订单号 / 金额
        std::string orderId = extractField(params,
            {"out_trade_no", "orderId", "order_id", "orderNo", "mchOrderNo"});
        std::string channelOrderNo = extractField(params,
            {"trade_no", "tradeNo", "transaction_id", "payOrderId"});
        std::string amountStr = extractField(params,
            {"total_amount", "money", "amount", "totalFee", "total_fee"});
        std::string buyerId = extractField(params,
            {"buyer_id", "buyer_logon_id", "openid"});

        if (orderId.empty()) {
            // 尝试从 body 原文解析
            std::string bodyStr(req->getBody().data(), req->getBody().size());
            std::cerr << "[Notify] " << channelCode << " 无法提取订单号, body=" << bodyStr << std::endl;
            textResp(cb, "fail: missing order_id"); return;
        }

        // 查订单
        auto order = db.queryOne("SELECT * FROM pay_order WHERE order_id=?", {orderId});
        if (order.empty() && !orderId.empty() && std::isdigit((unsigned char)orderId[0])) {
            // 兼容 EpayPlugin 等剥离 W 前缀的场景，反向尝试加上 W
            order = db.queryOne("SELECT * FROM pay_order WHERE order_id=?", {"W" + orderId});
            if (!order.empty()) orderId = "W" + orderId;
        }
        if (order.empty()) {
            textResp(cb, "fail: order not found"); return;
        }
        if (order["state"] == "1") {
            textResp(cb, "success"); return; // 已处理,幂等返回
        }
        // 已处理,幂等返回
        if (order["state"] != "0") { textResp(cb, "success"); return; }

        // 按通道动态验签（调用对应插件的 verifyNotify）
        if (!verifyChannelNotify(channelCode, params, req, order)) {
            std::cerr << "[Notify] " << channelCode << " 验签失败 orderId=" << orderId << std::endl;
            textResp(cb, "fail: signature verification failed"); return;
        }

        long long now = std::time(nullptr);
        std::string oid = order["order_id"];

        // 更新订单为已支付
        db.exec("UPDATE pay_order SET state=1,pay_time=?,buyer_id=?,"
                "channel_order_no=?,updated_at=? WHERE order_id=? AND state=0",
                {std::to_string(now), buyerId, channelOrderNo,
                 std::to_string(now), oid});

        // WebSocket 实时推送给前端订阅者
        {
            Json::Value evt;
            evt["order_id"] = oid;
            evt["state"]    = 1;
            evt["amount"]   = order["amount"];
            evt["pay_time"] = (Json::Int64)now;
            evt["msg"]      = "支付成功";
            WsBus::instance().publish("order:" + oid, evt);
        }

        // 清理金额锁
        db.exec("DELETE FROM tmp_price WHERE oid=?", {oid});

        // 商户入账: 订单金额 - 手续费
        int mchId = std::stoi(order["mch_id"]);
        double amount = 0, mchFee = 0;
        try { amount = std::stod(order["amount"]); } catch (...) {}
        try { mchFee = std::stod(order["mch_fee_amount"]); } catch (...) {}
        bool isSourceBiz = order.count("param") && order.at("param").rfind("source:", 0) == 0;
        double income = amount - mchFee;
        if (!isSourceBiz && income > 0 && mchId > 0) {
            ChannelService::changeMchBalance(mchId, 1, income, "order", oid, "订单收入");
            // 更新商户累计收入
            db.exec("UPDATE merchant SET total_income = "
                    "CAST(CAST(total_income AS REAL) + ? AS TEXT), updated_at=? WHERE id=?",
                    {ChannelService::fmtAmount(income), std::to_string(now),
                     std::to_string(mchId)});
        }

        handleSourceCompatBiz(order, now);

        // 异步通知商户
        std::string notifyUrl = order.count("notify_url") ? order["notify_url"] : "";
        if (!notifyUrl.empty() && mchId > 0) {
            auto mch = db.queryOne("SELECT mch_key FROM merchant WHERE id=?",
                                   {std::to_string(mchId)});
            std::string mchKey = mch.empty() ? "" : mch["mch_key"];
            std::string fullUrl = buildMchNotifyUrl(notifyUrl, order, mchKey);
            NotifyTaskService::createTaskAndSend(oid, fullUrl, "NotifyReceiveCtrl");
            db.exec("UPDATE pay_order SET notify_state=1 WHERE order_id=?", {oid});
        }

        textResp(cb, "success");
    }

    // 构建商户通知 URL
    static std::string buildMchNotifyUrl(const std::string &notifyUrl,
                                          const PayDb::Row &order,
                                          const std::string &mchKey) {
        // 兼容易支付格式
        std::map<std::string, std::string> m;
        m["trade_no"]      = order.at("order_id");
        m["out_trade_no"]  = order.count("mch_order_no") ? order.at("mch_order_no") : "";
        m["type"]          = order.count("pay_type") ? order.at("pay_type") : "";
        m["money"]         = order.count("amount") ? order.at("amount") : "";
        m["trade_status"]  = "TRADE_SUCCESS";
        if (order.count("param") && !order.at("param").empty())
            m["param"]     = order.at("param");
        if (order.count("subject"))
            m["name"]      = order.at("subject");

        std::string signStr = EpaySign::sign(m, mchKey);
        m["sign"]      = signStr;
        m["sign_type"] = "MD5";

        std::string query;
        for (auto &[k, v] : m) {
            if (!query.empty()) query += "&";
            query += k + "=" + urlEncode(v);
        }
        std::string sep = (notifyUrl.find('?') == std::string::npos) ? "?" : "&";
        return notifyUrl + sep + query;
    }

    static void handleSourceCompatBiz(const PayDb::Row &order, long long now) {
        auto it = order.find("param");
        if (it == order.end() || it->second.rfind("source:", 0) != 0) return;
        auto &db = PayDb::instance();
        std::string p = it->second;
        std::string prefixRecharge = "source:recharge:";
        std::string prefixVip = "source:vip:";
        std::string prefixRegister = "source:register:";
        if (p.rfind(prefixRecharge, 0) == 0) {
            std::string orderNo = p.substr(prefixRecharge.size());
            auto r = db.queryOne("SELECT * FROM recharge_order WHERE order_no=?", {orderNo});
            if (!r.empty() && r["state"] == "0") {
                int mchId = std::stoi(r["mch_id"]);
                double amount = 0; try { amount = std::stod(r["amount"]); } catch (...) {}
                if (amount > 0) ChannelService::changeMchBalance(mchId, 1, amount, "recharge", orderNo, "充值到账");
                db.exec("UPDATE recharge_order SET state=1,paid_at=? WHERE order_no=?",
                        {std::to_string(now), orderNo});
            }
        } else if (p.rfind(prefixVip, 0) == 0) {
            std::string orderNo = p.substr(prefixVip.size());
            auto r = db.queryOne("SELECT * FROM recharge_order WHERE order_no=?", {orderNo});
            if (!r.empty() && r["state"] == "0") {
                std::string vipId = "";
                if (r.count("pay_type") && r.at("pay_type").rfind("vip:", 0) == 0)
                    vipId = r.at("pay_type").substr(4);
                auto vip = db.queryOne("SELECT days FROM vip_package WHERE id=?", {vipId});
                int days = vip.empty() ? 30 : std::stoi(vip["days"]);
                long long cur = now;
                auto mch = db.queryOne("SELECT vip_expire_at FROM merchant WHERE id=?", {r["mch_id"]});
                if (!mch.empty()) {
                    try { cur = std::max(now, std::stoll(mch["vip_expire_at"])); } catch (...) {}
                }
                db.exec("UPDATE merchant SET vip_expire_at=?,updated_at=? WHERE id=?",
                        {std::to_string(cur + (long long)days * 86400), std::to_string(now), r["mch_id"]});
                db.exec("UPDATE recharge_order SET state=1,paid_at=? WHERE order_no=?",
                        {std::to_string(now), orderNo});
            }
        } else if (p.rfind(prefixRegister, 0) == 0) {
            std::string orderNo = p.substr(prefixRegister.size());
            auto ro = db.queryOne("SELECT * FROM register_order WHERE order_no=?", {orderNo});
            if (!ro.empty() && ro["state"] == "0") {
                auto exists = db.queryOne("SELECT id FROM merchant WHERE username=?", {ro["username"]});
                if (exists.empty()) {
                    std::string salt = PasswordUtils::generateSalt();
                    std::string pwd = ro["password"].empty() ? "123456" : ro["password"];
                    std::string hash = PasswordUtils::hashPassword(pwd, salt);
                    std::string key = PasswordUtils::generateKey(32);
                    std::string mchNo = "M" + std::to_string(now);
                    db.exec("INSERT INTO merchant(mch_no,username,password,salt,mch_name,contact,phone,email,mch_key,state,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
                        {mchNo, ro["username"], hash, salt, ro["mch_name"], ro["mch_name"], ro["phone"], ro["email"], key, "1", std::to_string(now), std::to_string(now)});
                    auto mch = db.queryOne("SELECT id FROM merchant WHERE username=?", {ro["username"]});
                    std::string mchId = mch.empty() ? "0" : mch["id"];
                    db.exec("UPDATE register_order SET state=1,mch_id=?,paid_at=? WHERE order_no=?",
                            {mchId, std::to_string(now), orderNo});
                }
            }
        }
    }

    // ── 通道回调签名校验 ─────────────────────────────────────────
    // 根据 channelCode 找到对应插件，调用其 verifyNotify 进行验签
    static bool verifyChannelNotify(const std::string &channelCode,
                                   const std::map<std::string, std::string> &params,
                                   const drogon::HttpRequestPtr &req,
                                   const PayDb::Row &order) {
        auto &db = PayDb::instance();
        int mchId = 0;
        try { mchId = std::stoi(order.at("mch_id")); } catch (...) {}
        if (mchId <= 0) return false;

        // 查找该通道配置（优先商户专属通道，次系统通道）
        auto channelRow = db.queryOne(
            "SELECT c.id,c.plugin,c.params_json FROM pay_channel c "
            "JOIN merchant_channel mc ON mc.channel_id=c.id "
            "WHERE mc.mch_id=? AND LOWER(c.channel_code)=LOWER(?) AND mc.state=1 AND c.state=1 "
            "ORDER BY mc.id DESC LIMIT 1",
            {std::to_string(mchId), channelCode});
        if (channelRow.empty()) {
            channelRow = db.queryOne(
                "SELECT id,plugin,params_json FROM pay_channel "
                "WHERE LOWER(channel_code)=LOWER(?) AND state=1 LIMIT 1",
                {channelCode});
        }
        if (channelRow.empty()) {
            LOG_ERROR << "[Notify] 通道不存在: " << channelCode;
            return false;
        }

        std::string pluginName = channelRow.count("plugin") ? channelRow.at("plugin") : "";
        Json::Value channelParams;
        if (channelRow.count("params_json") && !channelRow.at("params_json").empty()) {
            try {
                Json::CharReaderBuilder b;
                std::istringstream iss(channelRow.at("params_json"));
                std::string errs;
                Json::parseFromStream(b, iss, &channelParams, &errs);
            } catch (...) {}
        }

        // 特殊处理：Vmq/Wepay/V3 等继承 VmqPlugin 的插件，按金额+类型匹配
        if (pluginName == "VmqPlugin" || pluginName == "WepayPlugin" ||
            pluginName == "WepayV3Plugin" || channelCode == "wepay" ||
            channelCode == "vmq" || channelCode == "epay") {
            return verifyByAmountMatch(params, order);
        }

        // 构造插件并验签
        if (pluginName.empty()) {
            // 无插件名的通道（如纯回调通道），按金额+类型匹配
            return verifyByAmountMatch(params, order);
        }

        auto plugin = ChannelPluginRegistry::instance().create(pluginName);
        if (!plugin) {
            LOG_WARN << "[Notify] 插件创建失败: " << pluginName << "，降级为金额匹配验签";
            return verifyByAmountMatch(params, order);
        }

        std::string rawBody(req->getBody().data(), req->getBody().size());
        auto result = plugin->verifyNotify(params, rawBody, channelParams);
        if (!result.verified) {
            LOG_WARN << "[Notify] " << channelCode << " (" << pluginName << ") 验签失败";
            return false;
        }

        // 验签通过后，用插件返回的数据补充字段（如果插件返回了）
        if (!result.channelOrderNo.empty()) {
            // channelOrderNo 已通过 extractField 通用提取，此处可覆盖
        }

        return true;
    }

    // 降级策略：按支付类型+金额匹配订单（无签名通道兜底）
    static bool verifyByAmountMatch(const std::map<std::string, std::string> &params,
                                   const PayDb::Row &order) {
        std::string priceStr = extractField(params,
            {"total_amount", "money", "amount", "totalFee", "total_fee", "price"});
        double price = 0;
        try { price = std::stod(priceStr); } catch (...) {}
        double orderAmt = 0;
        try { orderAmt = std::stod(order.at("amount")); } catch (...) {}
        if (price > 0 && std::abs(price - orderAmt) < 0.01) {
            return true;  // 金额匹配，视为有效回调
        }
        LOG_WARN << "[Notify] 金额不匹配: callback=" << priceStr
                 << " order=" << order.at("amount");
        return false;
    }

    static std::string extractField(const std::map<std::string, std::string> &params,
                                     const std::vector<std::string> &keys) {
        for (auto &k : keys) {
            auto it = params.find(k);
            if (it != params.end() && !it->second.empty()) return it->second;
        }
        return "";
    }

    static void textResp(std::function<void(const drogon::HttpResponsePtr &)> &cb,
                         const std::string &text) {
        auto r = drogon::HttpResponse::newHttpResponse();
        r->setBody(text);
        cb(r);
    }

    static std::string urlEncode(const std::string &s) {
        std::ostringstream oss;
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') oss << c;
            else oss << '%' << std::uppercase << std::hex
                     << std::setw(2) << std::setfill('0') << (int)c;
        }
        return oss.str();
    }
};
