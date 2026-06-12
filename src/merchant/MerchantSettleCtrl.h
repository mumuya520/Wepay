// WePay-Cpp — 商户后台: 结算申请
// POST /merchant/api/settle/apply    申请结算（冻结金额）
// GET  /merchant/api/settle/list     结算记录（分页）
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // 时间库
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/ChannelService.h" // 通道服务
#include "../filters/MerchantAuthFilter.h" // 商户认证过滤器

// 商户结算控制器类
class MerchantSettleCtrl : public drogon::HttpController<MerchantSettleCtrl> {
public:
    METHOD_LIST_BEGIN // 路由列表开始
        ADD_METHOD_TO(MerchantSettleCtrl::apply, "/merchant/api/settle/apply", drogon::Post, "MerchantAuthFilter"); // 申请结算
        ADD_METHOD_TO(MerchantSettleCtrl::list,  "/merchant/api/settle/list",  drogon::Get,  "MerchantAuthFilter"); // 结算记录
    METHOD_LIST_END // 路由列表结束

    // 申请结算方法（冻结金额）
    void apply(const drogon::HttpRequestPtr &req, // HTTP 请求对象
               std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        auto body = req->getJsonObject(); // 获取 JSON 请求体
        if (!body) { RESP_ERR(cb, "格式错误"); return; } // 请求体不是 JSON

        std::string amountStr = (*body).get("amount", "0").asString(); // 获取结算金额字符串
        double amount = 0; // 结算金额
        try { amount = std::stod(amountStr); } catch (...) {} // 解析结算金额
        if (amount <= 0) { RESP_ERR(cb, "结算金额必须大于0"); return; } // 金额必须大于 0

        auto &db = PayDb::instance(); // 获取数据库单例
        auto mch = db.queryOne("SELECT balance,settle_account FROM merchant WHERE id=?", {mchId}); // 查询商户余额和结算账户
        if (mch.empty()) { RESP_ERR(cb, "商户不存在"); return; } // 商户不存在

        double balance = 0; // 商户余额
        try { balance = std::stod(mch["balance"]); } catch (...) {} // 解析余额
        if (amount > balance) { RESP_ERR(cb, "可用余额不足"); return; } // 余额不足

        // 检查是否有未处理的结算单
        auto pending = db.queryOne( // 查询未处理的结算单
            "SELECT id FROM settle_order WHERE mch_id=? AND state IN (0,1)", {mchId}); // 按商户 ID 查询状态为 0 或 1 的结算单
        if (!pending.empty()) { // 如果有未处理的结算单
            RESP_ERR(cb, "有未处理的结算申请，请等待处理后再提交"); return; // 返回错误
        }

        // 冻结金额
        if (!ChannelService::changeMchBalance(std::stoi(mchId), 3, amount, // 商户余额冻结（3=冻结）
                                               "settle_freeze", "", "结算冻结")) { // 业务类型、单号、备注
            RESP_ERR(cb, "余额冻结失败"); return; // 冻结失败
        }

        // 创建结算单(手续费暂设0, 后续可配置)
        std::string settleNo = ChannelService::generateSettleNo(); // 生成结算单号
        double fee = 0; // 手续费
        double realAmount = amount - fee; // 实际结算金额（扣除手续费）
        long long now = std::time(nullptr); // 获取当前时间戳

        db.exec("INSERT INTO settle_order(settle_no,mch_id,amount,fee,real_amount," // 插入结算单
                "account_info,state,created_at,updated_at) VALUES(?,?,?,?,?,?,0,?,?)",
                {settleNo, mchId, ChannelService::fmtAmount(amount), // 结算单号、商户 ID、结算金额
                 ChannelService::fmtAmount(fee), ChannelService::fmtAmount(realAmount), // 手续费、实际结算金额
                 mch["settle_account"], std::to_string(now), std::to_string(now)}); // 结算账户、创建时间、更新时间

        Json::Value data; // 响应数据
        data["settle_no"]   = settleNo; // 结算单号
        data["amount"]      = ChannelService::fmtAmount(amount); // 结算金额
        data["real_amount"] = ChannelService::fmtAmount(realAmount); // 实际结算金额
        RESP_OK(cb, data); // 返回成功响应
    }

    // 结算记录方法（分页）
    void list(const drogon::HttpRequestPtr &req, // HTTP 请求对象
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        int page = 1, size = 20; // 初始化页码和每页数量
        try { page = std::stoi(req->getParameter("page")); } catch (...) {} // 尝试解析页码
        try { size = std::stoi(req->getParameter("size")); } catch (...) {} // 尝试解析每页数量
        if (page < 1) page = 1; // 页码最小为 1
        int offset = (page - 1) * size; // 计算数据库偏移量

        auto &db = PayDb::instance(); // 获取数据库单例
        auto cntR = db.query("SELECT COUNT(*) AS c FROM settle_order WHERE mch_id=?", {mchId}); // 查询总数
        int total = cntR.empty() ? 0 : std::stoi(cntR[0]["c"]); // 获取总数

        auto rows = db.query( // 查询结算记录
            "SELECT * FROM settle_order WHERE mch_id=? ORDER BY id DESC LIMIT ? OFFSET ?", // 按 ID 倒序，分页查询
            {mchId, std::to_string(size), std::to_string(offset)}); // 商户 ID、分页参数

        Json::Value arr(Json::arrayValue); // 创建 JSON 数组
        for (auto &r : rows) { // 遍历每条结算记录
            Json::Value item; // 创建结算项
            item["id"]           = std::stoi(r["id"]); // 结算单 ID
            item["settle_no"]    = r["settle_no"]; // 结算单号
            item["amount"]       = r["amount"]; // 结算金额
            item["fee"]          = r["fee"]; // 手续费
            item["real_amount"]  = r["real_amount"]; // 实际结算金额
            item["state"]        = std::stoi(r["state"]); // 结算状态（转为整数）
            item["admin_remark"] = r["admin_remark"]; // 管理员备注
            item["created_at"]   = (Json::Int64)std::stoll(r["created_at"]); // 创建时间戳
            arr.append(item); // 添加到数组
        }
        Json::Value data; // 响应数据
        data["list"] = arr; // 结算记录列表
        data["total"] = total; // 总数
        RESP_OK(cb, data); // 返回成功响应
    }
};
