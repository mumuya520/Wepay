// WePay-Cpp — 商户端: Source 兼容模块
// 支持 Source 平台的各类功能（域名管理、工单、CDK、VIP、充值、公告等）
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // 时间库
#include <random> // 随机数生成
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/ChannelService.h" // 通道服务
#include "../channel/ChannelPlugin.h" // 通道插件接口
#include "../filters/MerchantAuthFilter.h" // 商户认证过滤器

// 商户 Source 兼容控制器类
class MerchantSourceCompatCtrl : public drogon::HttpController<MerchantSourceCompatCtrl> {
public:
    METHOD_LIST_BEGIN // 路由列表开始
        ADD_METHOD_TO(MerchantSourceCompatCtrl::domainList, "/merchant/api/source/domain/list", drogon::Get, "MerchantAuthFilter");
        ADD_METHOD_TO(MerchantSourceCompatCtrl::domainSave, "/merchant/api/source/domain/save", drogon::Post, "MerchantAuthFilter");
        ADD_METHOD_TO(MerchantSourceCompatCtrl::domainDelete, "/merchant/api/source/domain/delete", drogon::Post, "MerchantAuthFilter");
        ADD_METHOD_TO(MerchantSourceCompatCtrl::ticketList, "/merchant/api/source/ticket/list", drogon::Get, "MerchantAuthFilter");
        ADD_METHOD_TO(MerchantSourceCompatCtrl::ticketCategories, "/merchant/api/source/ticket/categories", drogon::Get, "MerchantAuthFilter");
        ADD_METHOD_TO(MerchantSourceCompatCtrl::ticketCreate, "/merchant/api/source/ticket/create", drogon::Post, "MerchantAuthFilter");
        ADD_METHOD_TO(MerchantSourceCompatCtrl::ticketClose, "/merchant/api/source/ticket/close", drogon::Post, "MerchantAuthFilter");
        ADD_METHOD_TO(MerchantSourceCompatCtrl::cdkRedeem, "/merchant/api/source/cdk/redeem", drogon::Post, "MerchantAuthFilter");
        ADD_METHOD_TO(MerchantSourceCompatCtrl::vipList, "/merchant/api/source/vip/list", drogon::Get, "MerchantAuthFilter");
        ADD_METHOD_TO(MerchantSourceCompatCtrl::vipBuy, "/merchant/api/source/vip/buy", drogon::Post, "MerchantAuthFilter");
        ADD_METHOD_TO(MerchantSourceCompatCtrl::rechargeCreate, "/merchant/api/source/recharge/create", drogon::Post, "MerchantAuthFilter");
        ADD_METHOD_TO(MerchantSourceCompatCtrl::rechargeList, "/merchant/api/source/recharge/list", drogon::Get, "MerchantAuthFilter");
        ADD_METHOD_TO(MerchantSourceCompatCtrl::noticeConfig, "/merchant/api/source/notice/config", drogon::Get, "MerchantAuthFilter");
        ADD_METHOD_TO(MerchantSourceCompatCtrl::noticeSave, "/merchant/api/source/notice/save", drogon::Post, "MerchantAuthFilter");
        ADD_METHOD_TO(MerchantSourceCompatCtrl::registerOrderCreate, "/merchant/api/source/register/order", drogon::Post);
        ADD_METHOD_TO(MerchantSourceCompatCtrl::registerOrderQuery, "/merchant/api/source/register/query", drogon::Get);
        ADD_METHOD_TO(MerchantSourceCompatCtrl::profileExt, "/merchant/api/source/profile/ext", drogon::Get, "MerchantAuthFilter");
        ADD_METHOD_TO(MerchantSourceCompatCtrl::realNameSubmit, "/merchant/api/source/realName/submit", drogon::Post, "MerchantAuthFilter");
        ADD_METHOD_TO(MerchantSourceCompatCtrl::googleBind, "/merchant/api/source/google/bind", drogon::Post, "MerchantAuthFilter");
        ADD_METHOD_TO(MerchantSourceCompatCtrl::googleUnbind, "/merchant/api/source/google/unbind", drogon::Post, "MerchantAuthFilter");
        ADD_METHOD_TO(MerchantSourceCompatCtrl::affiliateInfo, "/merchant/api/source/affiliate/info", drogon::Get, "MerchantAuthFilter");
    METHOD_LIST_END

    // 域名列表方法
    void domainList(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 获取商户域名列表
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        auto rows = PayDb::instance().query("SELECT * FROM merchant_domain WHERE mch_id=? ORDER BY id DESC", {mchId}); // 查询域名列表
        RESP_OK(cb, rowsToJson(rows)); // 返回成功响应
    }

    // 保存域名方法
    void domainSave(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 保存商户域名
        auto j = req->getJsonObject(); // 获取 JSON 请求体
        if (!j) { RESP_ERR(cb, "参数错误"); return; } // 参数错误
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        std::string siteUrl = cleanDomain((*j).get("site_url", "").asString()); // 清理域名
        if (siteUrl.empty()) { RESP_ERR(cb, "站点域名不能为空"); return; } // 域名为空
        long long now = std::time(nullptr); // 获取当前时间戳
        PayDb::instance().exec( // 插入域名
            "INSERT INTO merchant_domain(mch_id,site_name,site_url,state,remark,created_at,updated_at) VALUES(?,?,?,?,?,?,?)",
            {mchId, (*j).get("site_name", "").asString(), siteUrl, "0", "", std::to_string(now), std::to_string(now)}); // 插入参数
        RESP_MSG(cb, "已提交审核"); // 返回成功消息
    }

    // 删除域名方法
    void domainDelete(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 删除商户域名
        auto j = req->getJsonObject(); // 获取 JSON 请求体
        if (!j) { RESP_ERR(cb, "参数错误"); return; } // 参数错误
        PayDb::instance().exec("DELETE FROM merchant_domain WHERE id=? AND mch_id=?", // 删除域名
            {(*j).get("id", "").asString(), req->getHeader("X-Mch-Id")}); // 删除参数
        RESP_MSG(cb, "域名已删除"); // 返回成功消息
    }

    // 工单列表方法
    void ticketList(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 获取商户工单列表
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        auto rows = PayDb::instance().query("SELECT * FROM support_ticket WHERE mch_id=? ORDER BY id DESC", {mchId}); // 查询工单列表
        RESP_OK(cb, rowsToJson(rows)); // 返回成功响应
    }

    // 工单分类方法
    void ticketCategories(const drogon::HttpRequestPtr &, std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 获取工单分类
        auto rows = PayDb::instance().query("SELECT * FROM ticket_category WHERE state=1 ORDER BY sort_order ASC,id ASC", {}); // 查询工单分类
        RESP_OK(cb, rowsToJson(rows)); // 返回成功响应
    }

    // 创建工单方法
    void ticketCreate(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 创建工单
        auto j = req->getJsonObject(); // 获取 JSON 请求体
        if (!j) { RESP_ERR(cb, "参数错误"); return; } // 参数错误
        std::string title = (*j).get("title", "").asString(); // 获取工单标题
        std::string content = (*j).get("content", "").asString(); // 获取工单内容
        if (title.empty() || content.empty()) { RESP_ERR(cb, "标题和内容不能为空"); return; } // 标题或内容为空
        PayDb::instance().exec( // 插入工单
            "INSERT INTO support_ticket(mch_id,category,title,content,state,created_at) VALUES(?,?,?,?,0,?)",
            {req->getHeader("X-Mch-Id"), (*j).get("category", "").asString(), title, content, std::to_string(std::time(nullptr))}); // 插入参数
        RESP_MSG(cb, "工单已提交"); // 返回成功消息
    }

    // 关闭工单方法
    void ticketClose(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 关闭工单
        auto j = req->getJsonObject(); // 获取 JSON 请求体
        if (!j) { RESP_ERR(cb, "参数错误"); return; } // 参数错误
        PayDb::instance().exec("UPDATE support_ticket SET state=3 WHERE id=? AND mch_id=?", // 更新工单状态
            {(*j).get("id", "").asString(), req->getHeader("X-Mch-Id")}); // 更新参数
        RESP_MSG(cb, "工单已关闭"); // 返回成功消息
    }

    // CDK 兑换方法
    void cdkRedeem(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 兑换 CDK
        auto j = req->getJsonObject(); // 获取 JSON 请求体
        if (!j) { RESP_ERR(cb, "参数错误"); return; } // 参数错误
        std::string code = (*j).get("code", "").asString(); // 获取 CDK 代码
        if (code.empty()) { RESP_ERR(cb, "CDK不能为空"); return; } // CDK 为空
        auto &db = PayDb::instance(); // 获取数据库单例
        auto cdk = db.queryOne("SELECT * FROM cdk_code WHERE code=?", {code}); // 查询 CDK
        if (cdk.empty()) { RESP_ERR(cb, "CDK不存在"); return; } // CDK 不存在
        if (cdk["state"] != "0") { RESP_ERR(cb, "CDK已使用"); return; } // CDK 已使用
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        long long now = std::time(nullptr); // 获取当前时间戳
        if (cdk["cdk_type"] == "1") { // 如果是余额类 CDK
            double money = toDouble(cdk["value"]); // 转换金额
            ChannelService::changeMchBalance(std::stoi(mchId), 1, money, "cdk", code, "CDK兑换余额"); // 增加商户余额
        } else { // 否则是 VIP 类 CDK
            db.exec("INSERT OR REPLACE INTO mch_config(mch_id,config_key,config_value,remark,updated_at) VALUES(?,?,?,?,?)",
                    {mchId, "vip_package", cdk["value"], "CDK兑换VIP", std::to_string(now)}); // 保存 VIP 配置
        }
        db.exec("UPDATE cdk_code SET state=1,used_mch_id=?,used_at=? WHERE code=?", {mchId, std::to_string(now), code}); // 更新 CDK 状态
        RESP_MSG(cb, "兑换成功"); // 返回成功消息
    }

    // VIP 列表方法
    void vipList(const drogon::HttpRequestPtr &, std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 获取 VIP 套餐列表
        auto rows = PayDb::instance().query("SELECT * FROM vip_package WHERE state=1 ORDER BY price ASC,id ASC", {}); // 查询 VIP 套餐
        RESP_OK(cb, rowsToJson(rows)); // 返回成功响应
    }

    // VIP 购买方法
    void vipBuy(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 购买 VIP
        auto j = req->getJsonObject(); // 获取 JSON 请求体
        if (!j) { RESP_ERR(cb, "参数错误"); return; } // 参数错误
        std::string id = (*j).get("vip_id", "").asString(); // 获取 VIP ID
        auto vip = PayDb::instance().queryOne("SELECT * FROM vip_package WHERE id=? AND state=1", {id}); // 查询 VIP 套餐
        if (vip.empty()) { RESP_ERR(cb, "VIP套餐不存在"); return; } // VIP 不存在
        std::string orderNo = ChannelService::generateOrderId("VIP"); // 生成订单号
        PayDb::instance().exec("INSERT INTO recharge_order(order_no,mch_id,amount,pay_type,state,created_at) VALUES(?,?,?,?,0,?)", // 插入充值订单
            {orderNo, req->getHeader("X-Mch-Id"), vip["price"], "vip:" + id, std::to_string(std::time(nullptr))}); // 插入参数
        std::string payOrderId, payUrl; // 支付订单 ID 和 URL
        createPluginPayOrder(req, req->getHeader("X-Mch-Id"), orderNo, vip["price"], // 创建支付订单
                             (*j).get("pay_type", "wxpay").asString(), "VIP购买", "source:vip:" + orderNo, payOrderId, payUrl); // 支付参数
        Json::Value data; // 响应数据
        data["order_no"] = orderNo; // 订单号
        data["vip_id"] = id; // VIP ID
        data["amount"] = vip["price"]; // 金额
        data["trade_no"] = payOrderId; // 支付订单号
        data["pay_url"] = payUrl.empty() ? ("/gateway/cashier/" + payOrderId) : payUrl; // 支付 URL
        RESP_OK(cb, data); // 返回成功响应
    }

    // 充值订单创建方法
    void rechargeCreate(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 创建充值订单
        auto j = req->getJsonObject(); // 获取 JSON 请求体
        if (!j) { RESP_ERR(cb, "参数错误"); return; } // 参数错误
        double amount = toDouble((*j).get("amount", "0").asString()); // 获取充值金额
        if (amount <= 0) { RESP_ERR(cb, "充值金额必须大于0"); return; } // 金额无效
        std::string orderNo = ChannelService::generateOrderId("RCG"); // 生成订单号
        PayDb::instance().exec("INSERT INTO recharge_order(order_no,mch_id,amount,pay_type,state,created_at) VALUES(?,?,?,?,0,?)", // 插入充值订单
            {orderNo, req->getHeader("X-Mch-Id"), ChannelService::fmtAmount(amount), (*j).get("pay_type", "").asString(), std::to_string(std::time(nullptr))}); // 插入参数
        std::string payOrderId, payUrl; // 支付订单 ID 和 URL
        createPluginPayOrder(req, req->getHeader("X-Mch-Id"), orderNo, ChannelService::fmtAmount(amount), // 创建支付订单
                             (*j).get("pay_type", "wxpay").asString(), "商户充值", "source:recharge:" + orderNo, payOrderId, payUrl); // 支付参数
        Json::Value data; // 响应数据
        data["order_no"] = orderNo; // 订单号
        data["amount"] = ChannelService::fmtAmount(amount); // 金额
        data["trade_no"] = payOrderId; // 支付订单号
        data["pay_url"] = payUrl.empty() ? ("/gateway/cashier/" + payOrderId) : payUrl; // 支付 URL
        RESP_OK(cb, data); // 返回成功响应
    }

    // 充值记录方法
    void rechargeList(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 获取充值记录
        auto rows = PayDb::instance().query("SELECT * FROM recharge_order WHERE mch_id=? ORDER BY id DESC", {req->getHeader("X-Mch-Id")}); // 查询充值记录
        RESP_OK(cb, rowsToJson(rows)); // 返回成功响应
    }

    // 通知配置方法
    void noticeConfig(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 获取通知配置
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        auto rows = PayDb::instance().query("SELECT config_key,config_value,remark FROM mch_config WHERE mch_id=? AND config_key LIKE 'notice_%' ORDER BY config_key", {mchId}); // 查询通知配置
        RESP_OK(cb, rowsToJson(rows)); // 返回成功响应
    }

    // 保存通知配置方法
    void noticeSave(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 保存通知配置
        auto j = req->getJsonObject(); // 获取 JSON 请求体
        if (!j) { RESP_ERR(cb, "参数错误"); return; } // 参数错误
        auto &db = PayDb::instance(); // 获取数据库单例
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        long long now = std::time(nullptr); // 获取当前时间戳
        for (const auto &key : j->getMemberNames()) { // 遍历所有配置键
            if (key.rfind("notice_", 0) != 0) continue; // 仅处理 notice_ 前缀的配置
            db.exec("INSERT OR REPLACE INTO mch_config(mch_id,config_key,config_value,remark,updated_at) VALUES(?,?,?,?,?)", // 插入或更新配置
                {mchId, key, (*j)[key].asString(), "源支付通知配置", std::to_string(now)}); // 插入参数
        }
        RESP_MSG(cb, "通知配置已保存"); // 返回成功消息
    }

    // 创建注册订单方法
    void registerOrderCreate(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 创建注册订单
        auto j = req->getJsonObject(); // 获取 JSON 请求体
        if (!j) { RESP_ERR(cb, "参数错误"); return; } // 参数错误
        std::string username = (*j).get("username", "").asString(); // 获取用户名
        if (username.empty()) { RESP_ERR(cb, "用户名不能为空"); return; } // 用户名为空
        auto exists = PayDb::instance().queryOne("SELECT id FROM merchant WHERE username=?", {username}); // 查询用户名是否存在
        if (!exists.empty()) { RESP_ERR(cb, "用户名已存在"); return; } // 用户名已存在
        std::string orderNo = ChannelService::generateOrderId("REG"); // 生成订单号
        std::string price = PayDb::instance().getSetting("paid_reg_price", "0.00"); // 获取注册价格
        PayDb::instance().exec("INSERT INTO register_order(order_no,username,password,mch_name,email,phone,amount,state,created_at) VALUES(?,?,?,?,?,?,?,0,?)", // 插入注册订单
            {orderNo, username, (*j).get("password", "").asString(), (*j).get("mch_name", username).asString(), (*j).get("email", "").asString(), (*j).get("phone", "").asString(), price, std::to_string(std::time(nullptr))}); // 插入参数
        std::string payOrderId, payUrl; // 支付订单 ID 和 URL
        std::string sysMch = PayDb::instance().getSetting("paid_reg_mch_id", "1"); // 获取系统商户 ID
        createPluginPayOrder(req, sysMch, orderNo, price, (*j).get("pay_type", "wxpay").asString(), // 创建支付订单
                             "付费注册", "source:register:" + orderNo, payOrderId, payUrl); // 支付参数
        Json::Value data; // 响应数据
        data["order_no"] = orderNo; // 订单号
        data["amount"] = price; // 金额
        data["trade_no"] = payOrderId; // 支付订单号
        data["pay_url"] = payUrl.empty() ? ("/gateway/cashier/" + payOrderId) : payUrl; // 支付 URL
        RESP_OK(cb, data); // 返回成功响应
    }

    // 查询注册订单方法
    void registerOrderQuery(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 查询注册订单
        std::string orderNo = req->getParameter("order_no"); // 获取订单号参数
        auto row = PayDb::instance().queryOne("SELECT order_no,username,amount,state,mch_id,created_at,paid_at FROM register_order WHERE order_no=?", {orderNo}); // 查询注册订单
        if (row.empty()) { RESP_ERR(cb, "注册订单不存在"); return; } // 订单不存在
        Json::Value data; for (auto &[k, v] : row) data[k] = v; RESP_OK(cb, data); // 返回成功响应
    }

    // 扩展资料方法
    void profileExt(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 获取扩展资料
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        auto mch = PayDb::instance().queryOne("SELECT id,mch_no,mch_name,balance,is_real_name,google_secret,invite_mch_id FROM merchant WHERE id=?", {mchId}); // 查询商户信息
        Json::Value data; // 响应数据
        for (auto &[k, v] : mch) data[k] = v; // 复制所有字段
        data["google_bound"] = !mch["google_secret"].empty(); // 谷歌验证是否绑定
        RESP_OK(cb, data); // 返回成功响应
    }

    // 实名认证方法
    void realNameSubmit(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 提交实名认证
        auto j = req->getJsonObject(); // 获取 JSON 请求体
        if (!j) { RESP_ERR(cb, "参数错误"); return; } // 参数错误
        PayDb::instance().exec("UPDATE merchant SET is_real_name=1,contact=?,phone=?,updated_at=? WHERE id=?", // 更新实名认证信息
            {(*j).get("real_name", "").asString(), (*j).get("id_card", "").asString(), std::to_string(std::time(nullptr)), req->getHeader("X-Mch-Id")}); // 更新参数
        RESP_MSG(cb, "实名认证信息已保存"); // 返回成功消息
    }

    // 谷歌验证绑定方法
    void googleBind(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 绑定谷歌验证
        auto j = req->getJsonObject(); // 获取 JSON 请求体
        if (!j) { RESP_ERR(cb, "参数错误"); return; } // 参数错误
        std::string secret = (*j).get("secret", "").asString(); // 获取密钥
        if (secret.empty()) secret = makeSecret(); // 如果为空，生成新密钥
        PayDb::instance().exec("UPDATE merchant SET google_secret=?,updated_at=? WHERE id=?", // 更新谷歌密钥
            {secret, std::to_string(std::time(nullptr)), req->getHeader("X-Mch-Id")}); // 更新参数
        Json::Value data; data["secret"] = secret; data["bound"] = true; RESP_OK(cb, data); // 返回成功响应
    }

    // 谷歌验证解绑方法
    void googleUnbind(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 解绑谷歌验证
        PayDb::instance().exec("UPDATE merchant SET google_secret='',updated_at=? WHERE id=?", // 清除谷歌密钥
            {std::to_string(std::time(nullptr)), req->getHeader("X-Mch-Id")}); // 更新参数
        RESP_MSG(cb, "谷歌验证已解绑"); // 返回成功消息
    }

    // 推荐信息方法
    void affiliateInfo(const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 获取推荐信息
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        auto subs = PayDb::instance().query("SELECT id,mch_no,mch_name,created_at FROM merchant WHERE invite_mch_id=? ORDER BY id DESC", {mchId}); // 查询下级商户
        Json::Value data; // 响应数据
        data["invite_url"] = "/merchant/register?invite=" + mchId; // 推荐 URL
        data["sub_count"] = (Json::Int64)subs.size(); // 下级商户数
        data["subs"] = rowsToJson(subs); // 下级商户列表
        RESP_OK(cb, data); // 返回成功响应
    }

private:
    static Json::Value rowsToJson(const std::vector<PayDb::Row> &rows) {
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) { Json::Value it; for (auto &[k, v] : r) it[k] = v; arr.append(it); }
        return arr;
    }
    static std::string cleanDomain(std::string s) {
        const std::string h1 = "http://", h2 = "https://";
        if (s.rfind(h1, 0) == 0) s = s.substr(h1.size());
        if (s.rfind(h2, 0) == 0) s = s.substr(h2.size());
        while (!s.empty() && s.back() == '/') s.pop_back();
        return s;
    }
    static double toDouble(const std::string &s) { try { return std::stod(s); } catch (...) { return 0.0; } }
    static bool createPluginPayOrder(const drogon::HttpRequestPtr &req,
                                     const std::string &mchIdStr,
                                     const std::string &bizNo,
                                     const std::string &amountStr,
                                     const std::string &payType,
                                     const std::string &subject,
                                     const std::string &param,
                                     std::string &payOrderId,
                                     std::string &payUrl) {
        auto &db = PayDb::instance();
        int mchId = 0; try { mchId = std::stoi(mchIdStr); } catch (...) {}
        double amount = toDouble(amountStr);
        if (mchId <= 0 || amount <= 0) return false;
        auto channel = ChannelService::selectChannel(mchId, payType, amount);
        if (channel.channelId == 0) return false;
        double mchRate = ChannelService::getMchRate(mchId, channel.channelId);
        double mchFee = ChannelService::calcFee(amount, mchRate);
        double channelFee = ChannelService::calcFee(amount, channel.rate);
        long long now = std::time(nullptr);
        long long expire = now + 600;
        payOrderId = ChannelService::generateOrderId("SRC");
        bool ok = db.exec(
            "INSERT INTO pay_order(order_id,mch_id,app_id,mch_order_no,channel_id,pay_type,amount,real_amount,"
            "mch_fee_rate,mch_fee_amount,channel_fee_rate,channel_fee_amount,subject,body,param,notify_url,return_url,"
            "client_ip,state,notify_state,expire_time,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,0,0,?,?,?)",
            {payOrderId, std::to_string(mchId), "", bizNo, std::to_string(channel.channelId), payType,
             ChannelService::fmtAmount(amount), ChannelService::fmtAmount(amount),
             ChannelService::fmtAmount(mchRate), ChannelService::fmtAmount(mchFee),
             ChannelService::fmtAmount(channel.rate), ChannelService::fmtAmount(channelFee),
             subject, "", param, "", "", req->getPeerAddr().toIp(),
             std::to_string(expire), std::to_string(now), std::to_string(now)});
        if (!ok) return false;
        Json::Value channelParams;
        if (!channel.paramsJson.empty()) Json::Reader().parse(channel.paramsJson, channelParams);
        channelParams["plugin_code"] = channel.plugin;
        channelParams["channel_id"] = channel.channelId;
        channelParams["mch_id"] = mchId;
        auto plugin = ChannelPluginRegistry::instance().create(channel.plugin);
        if (!plugin) { payUrl = "/gateway/cashier/" + payOrderId; return true; }
        ChannelOrderRequest creq;
        creq.orderId = payOrderId;
        creq.mchOrderNo = bizNo;
        creq.amount = amount;
        creq.subject = subject;
        creq.notifyUrl = requestBaseUrl(req) + "/notify/channel/" + channel.plugin;
        creq.returnUrl = "";
        creq.clientIp = req->getPeerAddr().toIp();
        creq.payType = payType;
        creq.channelParams = channelParams;
        auto cres = plugin->createOrder(creq);
        if (!cres.success) return true;
        payUrl = !cres.payUrl.empty() ? cres.payUrl : (!cres.qrCode.empty() ? cres.qrCode : "/gateway/cashier/" + payOrderId);
        db.exec("UPDATE pay_order SET pay_url=?,qrcode=?,channel_order_no=?,raw_response=?,updated_at=? WHERE order_id=?",
                {cres.payUrl, cres.qrCode, cres.channelOrderNo, cres.rawResponse, std::to_string(std::time(nullptr)), payOrderId});
        return true;
    }
    static std::string requestBaseUrl(const drogon::HttpRequestPtr &req) {
        std::string host = req->getHeader("host");
        if (host.empty()) host = "127.0.0.1";
        std::string proto = req->getHeader("x-forwarded-proto");
        if (proto.empty()) proto = "http";
        return proto + "://" + host;
    }
    static std::string makeSecret() {
        static const char *cs = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
        static std::mt19937 rng((unsigned)std::random_device{}());
        std::string s; for (int i = 0; i < 16; ++i) s.push_back(cs[rng() % 32]); return s;
    }
};
