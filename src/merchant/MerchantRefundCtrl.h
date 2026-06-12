// WePay-Cpp — 商户后台: 自助退款
// POST /merchant/api/refund/apply    申请退款（支持部分退款）
// GET  /merchant/api/refund/list     退款记录（分页、筛选）
// GET  /merchant/api/refund/detail   退款详情
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // 时间库
#include <random> // 随机数生成
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/ChannelService.h" // 通道服务
#include "../channel/ChannelPlugin.h" // 通道插件接口
#include "../filters/MerchantAuthFilter.h" // 商户认证过滤器

// 商户退款控制器类
class MerchantRefundCtrl : public drogon::HttpController<MerchantRefundCtrl> {
public:
    METHOD_LIST_BEGIN // 路由列表开始
        ADD_METHOD_TO(MerchantRefundCtrl::apply,  "/merchant/api/refund/apply",  drogon::Post, "MerchantAuthFilter"); // 申请退款
        ADD_METHOD_TO(MerchantRefundCtrl::list,   "/merchant/api/refund/list",   drogon::Get,  "MerchantAuthFilter"); // 退款记录
        ADD_METHOD_TO(MerchantRefundCtrl::detail, "/merchant/api/refund/detail", drogon::Get,  "MerchantAuthFilter"); // 退款详情
    METHOD_LIST_END // 路由列表结束

    // 申请退款方法（支持部分退款）
    void apply(const drogon::HttpRequestPtr &req, // HTTP 请求对象
               std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        auto body = req->getJsonObject(); // 获取 JSON 请求体
        if (!body) { RESP_ERR(cb, "格式错误"); return; } // 请求体不是 JSON
        auto &j = *body; // 引用 JSON 对象

        std::string orderId = j.get("order_id", "").asString(); // 获取订单 ID
        double amount = 0; // 退款金额
        try { amount = std::stod(j.get("amount", "0").asString()); } catch(...) {} // 解析退款金额
        std::string reason = j.get("reason", "").asString(); // 获取退款原因

        if (orderId.empty() || amount <= 0) { // 检查必填参数
            RESP_ERR(cb, "order_id 和 amount 必填"); return; // 参数不完整
        }

        auto &db = PayDb::instance(); // 获取数据库单例
        auto order = db.queryOne( // 查询订单信息
            "SELECT id,mch_id,channel_id,amount,real_amount,state,pay_type " // 查询订单基本信息
            "FROM pay_order WHERE order_id=? AND mch_id=?", // 按订单 ID 和商户 ID 查询
            {orderId, mchId}); // 参数
        if (order.empty()) { RESP_ERR(cb, "订单不存在"); return; } // 订单不存在
        if (order["state"] != "1") { RESP_ERR(cb, "仅已支付订单可退款"); return; } // 订单未支付

        double paidAmt = 0; // 订单支付金额
        try { paidAmt = std::stod(order["amount"]); } catch(...) {} // 解析订单金额
        if (amount > paidAmt) { RESP_ERR(cb, "退款金额超过订单金额"); return; } // 退款金额超过订单金额

        // 累计已退款金额
        auto refunded = db.queryOne( // 查询已退款总额
            "SELECT COALESCE(SUM(CAST(refund_amount AS REAL)),0) AS total " // 查询累计退款金额
            "FROM refund_order WHERE order_id=? AND state IN (0,1)", {orderId}); // 按订单 ID 查询处理中或已完成的退款
        double already = 0; // 已退款金额
        if (!refunded.empty()) { try { already = std::stod(refunded["total"]); } catch(...) {} } // 解析已退款金额
        if (already + amount > paidAmt + 0.001) { // 检查累计退款金额（允许 0.001 元误差）
            RESP_ERR(cb, "累计退款金额不能超过订单金额"); return; // 累计退款超过订单金额
        }

        std::string refundNo = generateRefundNo(); // 生成退款单号
        long long now = std::time(nullptr); // 获取当前时间戳

        // 创建退款单(状态: 0=处理中)
        db.exec("INSERT INTO refund_order(refund_no,order_id,mch_id,channel_id," // 插入退款单
                "refund_amount,reason,state,created_at) VALUES(?,?,?,?,?,?,0,?)",
                {refundNo, orderId, mchId, order["channel_id"], // 退款单号、订单 ID、商户 ID、通道 ID
                 ChannelService::fmtAmount(amount), reason, // 退款金额、退款原因
                 std::to_string(now)}); // 创建时间

        // 加载通道插件并调用真实退款
        std::string channelId = order["channel_id"]; // 获取通道 ID
        bool localOnly = false; // 是否仅本地退款
        ChannelRefundResult chanResult; // 通道退款结果
        if (channelId.empty() || channelId == "0") { // 如果没有绑定通道
            localOnly = true;  // 仅本地账务退款
        } else { // 否则调用通道插件
            auto ch = db.queryOne("SELECT plugin,params_json FROM pay_channel WHERE id=?", {channelId}); // 查询通道信息
            if (!ch.empty()) { // 如果通道存在
                auto plugin = ChannelPluginRegistry::instance().create(ch["plugin"]); // 创建通道插件实例
                if (plugin) { // 如果插件创建成功
                    Json::Value cp; Json::Reader().parse(ch["params_json"], cp); // 解析通道参数
                    ChannelRefundRequest rr; // 创建退款请求
                    rr.refundNo       = refundNo; // 退款单号
                    rr.channelOrderNo = order["channel_order_no"]; // 通道订单号
                    rr.orderId        = orderId; // 订单 ID
                    rr.paidAmount     = paidAmt; // 支付金额
                    rr.refundAmount   = amount; // 退款金额
                    rr.reason         = reason; // 退款原因
                    rr.notifyUrl      = "/notify/refund/" + ch["plugin"]; // 回调地址
                    rr.channelParams  = cp; // 通道参数
                    chanResult = plugin->refund(rr); // 调用通道退款接口
                } else { // 插件创建失败
                    localOnly = true; // 降级为本地退款
                }
            } else { // 通道不存在
                localOnly = true; // 降级为本地退款
            }
        }

        // 处理结果
        bool finalOk = false; // 最终处理结果
        if (localOnly) { // 如果是本地退款
            // 本地账务退款
            finalOk = ChannelService::changeMchBalance(std::stoi(mchId), 2, amount, // 商户余额增加（2=收入）
                "refund", refundNo, "商户退款(本地)"); // 业务类型、单号、备注
            if (finalOk) { // 如果本地退款成功
                db.exec("UPDATE refund_order SET state=1,channel_refund_no=?,finished_at=? WHERE refund_no=?", // 更新退款单状态为已完成
                        {refundNo, std::to_string(now), refundNo}); // 通道退款号、完成时间、退款单号
            }
        } else { // 否则处理通道退款结果
            if (chanResult.success && chanResult.state >= 0) { // 如果通道退款成功
                // 通道返回成功(同步/异步)，更新本地账务
                ChannelService::changeMchBalance(std::stoi(mchId), 2, amount, // 商户余额增加
                    "refund", refundNo, "通道退款"); // 业务类型、单号、备注
                db.exec("UPDATE refund_order SET state=?,channel_refund_no=?,finished_at=? WHERE refund_no=?", // 更新退款单
                        {std::to_string(chanResult.state == 1 ? 1 : 0), // 状态：1=已完成，0=处理中
                         chanResult.channelRefundNo, // 通道退款号
                         std::to_string(chanResult.state == 1 ? now : 0), refundNo}); // 完成时间（仅状态为 1 时设置）
                finalOk = true; // 标记为成功
            } else { // 通道退款失败
                db.exec("UPDATE refund_order SET state=-1,err_msg=? WHERE refund_no=?", // 更新退款单状态为失败
                        {chanResult.errMsg, refundNo}); // 错误信息、退款单号
                RESP_ERR(cb, "通道退款失败: " + chanResult.errMsg); return; // 返回错误
            }
        }

        Json::Value data; // 响应数据
        data["refund_no"]      = refundNo; // 退款单号
        data["amount"]         = ChannelService::fmtAmount(amount); // 退款金额
        data["channel_refund_no"] = chanResult.channelRefundNo; // 通道退款号
        data["state"]          = chanResult.state; // 退款状态
        RESP_OK(cb, data); // 返回成功响应
    }

    // 退款记录方法（分页、筛选）
    void list(const drogon::HttpRequestPtr &req, // HTTP 请求对象
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        int page = pi(req->getParameter("page"), 1); // 获取页码（默认 1）
        int size = pi(req->getParameter("size"), 20); // 获取每页数量（默认 20）
        std::string state = req->getParameter("state"); // 获取状态筛选参数

        auto &db = PayDb::instance(); // 获取数据库单例
        std::string where = "mch_id=?"; // WHERE 子句初始化
        std::vector<std::string> params = {mchId}; // 参数列表
        if (!state.empty()) { where += " AND state=?"; params.push_back(state); } // 如果有状态筛选，添加到 WHERE

        auto cntR = db.query("SELECT COUNT(*) AS c FROM refund_order WHERE " + where, params); // 查询总数
        int total = cntR.empty() ? 0 : std::stoi(cntR[0]["c"]); // 获取总数
        auto pp = params; // 复制参数列表
        pp.push_back(std::to_string(size)); // 添加 LIMIT 参数
        pp.push_back(std::to_string((page - 1) * size)); // 添加 OFFSET 参数
        auto rows = db.query("SELECT * FROM refund_order WHERE " + where + // 查询退款记录
                              " ORDER BY id DESC LIMIT ? OFFSET ?", pp); // 按 ID 倒序，分页查询
        Json::Value arr(Json::arrayValue); // 创建 JSON 数组
        for (auto &r : rows) { // 遍历每条退款记录
            Json::Value it; // 创建退款项
            for (auto &[k, v] : r) it[k] = v; // 将查询结果转换为 JSON
            it["id"] = std::stoi(r["id"]); // 将 ID 转为整数
            it["mch_id"] = std::stoi(r["mch_id"]); // 将商户 ID 转为整数
            it["state"] = std::stoi(r["state"]); // 将状态转为整数
            it["created_at"] = (Json::Int64)std::stoll(r["created_at"]); // 将创建时间转为 Int64
            arr.append(it); // 添加到数组
        }
        Json::Value data; // 响应数据
        data["list"] = arr; // 退款记录列表
        data["total"] = total; // 总数
        RESP_OK(cb, data); // 返回成功响应
    }

    // 退款详情方法
    void detail(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        std::string no = req->getParameter("refund_no"); // 获取退款单号参数
        auto r = PayDb::instance().queryOne( // 查询退款详情
            "SELECT * FROM refund_order WHERE refund_no=? AND mch_id=?", {no, mchId}); // 按退款单号和商户 ID 查询
        if (r.empty()) { RESP_ERR(cb, "退款单不存在"); return; } // 退款单不存在
        Json::Value data; // 响应数据
        for (auto &[k, v] : r) data[k] = v; // 将查询结果转换为 JSON
        data["state"] = std::stoi(r["state"]); // 将状态转为整数
        RESP_OK(cb, data); // 返回退款详情
    }

private:
    // 安全字符串转整数方法
    static int pi(const std::string &s, int def) { // 字符串和默认值
        try { return std::stoi(s); } catch(...) { return def; } // 尝试转换，失败返回默认值
    }
    // 生成退款单号方法
    static std::string generateRefundNo() { // 返回退款单号
        static std::mt19937 rng((unsigned)std::random_device{}()); // 初始化随机数生成器
        long long ts = std::time(nullptr); // 获取当前时间戳
        char buf[32]; // 缓冲区
        std::snprintf(buf, sizeof(buf), "R%lld%04d", ts, (int)(rng() % 10000)); // 格式：R + 时间戳 + 4位随机数
        return buf; // 返回退款单号
    }
};
