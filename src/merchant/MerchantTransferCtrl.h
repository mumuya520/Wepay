// WePay-Cpp — 商户后台: 主动发起转账
// POST /merchant/api/transfer/create    发起转账（支持微信、支付宝、银行卡）
// GET  /merchant/api/transfer/list      转账记录（分页）
// GET  /merchant/api/transfer/detail    转账详情
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // 时间库
#include <random> // 随机数生成
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/ChannelService.h" // 通道服务
#include "../channel/ChannelPlugin.h" // 通道插件接口
#include "../filters/MerchantAuthFilter.h" // 商户认证过滤器

// 商户转账控制器类
class MerchantTransferCtrl : public drogon::HttpController<MerchantTransferCtrl> {
public:
    METHOD_LIST_BEGIN // 路由列表开始
        ADD_METHOD_TO(MerchantTransferCtrl::create, "/merchant/api/transfer/create", drogon::Post, "MerchantAuthFilter"); // 发起转账
        ADD_METHOD_TO(MerchantTransferCtrl::list,   "/merchant/api/transfer/list",   drogon::Get,  "MerchantAuthFilter"); // 转账记录
        ADD_METHOD_TO(MerchantTransferCtrl::detail, "/merchant/api/transfer/detail", drogon::Get,  "MerchantAuthFilter"); // 转账详情
    METHOD_LIST_END // 路由列表结束

    // 发起转账方法（支持微信、支付宝、银行卡）
    void create(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        auto body = req->getJsonObject(); // 获取 JSON 请求体
        if (!body) { RESP_ERR(cb, "格式错误"); return; } // 请求体不是 JSON
        auto &j = *body; // 引用 JSON 对象

        double amount = 0; // 转账金额
        try { amount = std::stod(j.get("amount", "0").asString()); } catch(...) {} // 解析转账金额
        if (amount <= 0) { RESP_ERR(cb, "金额必须大于0"); return; } // 金额必须大于 0

        std::string accountNo = j.get("account_no", "").asString(); // 获取收款账号
        if (accountNo.empty()) { RESP_ERR(cb, "收款账号必填"); return; } // 收款账号为空

        int accountType = j.get("account_type", 1).asInt();  // 获取账户类型（1=微信 2=支付宝 3=银行卡）

        auto &db = PayDb::instance(); // 获取数据库单例
        auto mch = db.queryOne("SELECT balance FROM merchant WHERE id=?", {mchId}); // 查询商户余额
        if (mch.empty()) { RESP_ERR(cb, "商户不存在"); return; } // 商户不存在

        double balance = 0; // 商户余额
        try { balance = std::stod(mch["balance"]); } catch(...) {} // 解析余额
        if (amount > balance) { RESP_ERR(cb, "商户余额不足"); return; } // 余额不足

        // 扣减商户余额(冻结转账金额)
        if (!ChannelService::changeMchBalance(std::stoi(mchId), 2, amount, // 商户余额扣减（2=支出）
            "transfer", "", "商户主动转账")) { // 业务类型、单号、备注
            RESP_ERR(cb, "扣款失败"); return; // 扣款失败
        }

        std::string transferNo = generateTransferNo(); // 生成转账单号
        double fee = 0;  // 商户转账费率可配置
        double real = amount - fee; // 实际转账金额（扣除费用）
        long long now = std::time(nullptr); // 获取当前时间戳

        db.exec("INSERT INTO transfer_order(transfer_no,mch_id,mch_transfer_no,amount,fee," // 插入转账单
                "real_amount,account_type,account_no,account_name,remark,state,created_at) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,0,?)",
                {transferNo, mchId, // 转账单号、商户 ID
                 j.get("mch_transfer_no", "").asString(), // 商户转账号
                 ChannelService::fmtAmount(amount), // 转账金额
                 ChannelService::fmtAmount(fee), // 手续费
                 ChannelService::fmtAmount(real), // 实际转账金额
                 std::to_string(accountType), // 账户类型
                 accountNo, // 收款账号
                 j.get("account_name", "").asString(), // 收款人姓名
                 j.get("remark", "").asString(), // 备注
                 std::to_string(now)}); // 创建时间

        // 尝试通过通道插件真正打款
        int transferState = 0;  // 转账状态（0=处理中）
        std::string channelTransferNo; // 通道转账号
        std::string errMsg; // 错误信息

        auto transferChannel = findTransferChannel(db, mchId, accountType); // 查找可用的转账通道
        if (!transferChannel.empty()) { // 如果找到转账通道
            auto plugin = ChannelPluginRegistry::instance().create(transferChannel["plugin"]); // 创建通道插件实例
            if (plugin) { // 如果插件创建成功
                Json::Value cp; // 通道参数
                Json::Reader().parse(transferChannel["params_json"], cp); // 解析通道参数

                ChannelTransferRequest tr; // 创建转账请求
                tr.transferNo   = transferNo; // 转账单号
                tr.amount       = real; // 实际转账金额
                tr.accountType  = accountType; // 账户类型
                tr.accountNo    = accountNo; // 收款账号
                tr.accountName  = j.get("account_name", "").asString(); // 收款人姓名
                tr.remark       = j.get("remark", "商户提现").asString(); // 备注
                tr.notifyUrl    = "/notify/transfer/" + transferChannel["plugin"]; // 回调地址
                tr.channelParams = cp; // 通道参数

                auto result = plugin->transfer(tr); // 调用通道转账接口
                if (result.success) { // 如果转账成功
                    transferState = result.state;  // 获取转账状态（0=处理中(异步) 1=成功）
                    channelTransferNo = result.channelTransferNo; // 获取通道转账号
                    long long now2 = std::time(nullptr); // 获取当前时间戳
                    db.exec("UPDATE transfer_order SET state=?,channel_transfer_no=?," // 更新转账单
                            "channel_id=?,finished_at=? WHERE transfer_no=?",
                            {std::to_string(transferState), // 转账状态
                             channelTransferNo, // 通道转账号
                             transferChannel["id"], // 通道 ID
                             std::to_string(transferState == 1 ? now2 : 0), // 完成时间（仅状态为 1 时设置）
                             transferNo}); // 转账单号
                } else { // 转账失败
                    errMsg = result.errMsg; // 获取错误信息
                    // 打款失败，退回余额
                    ChannelService::changeMchBalance(std::stoi(mchId), 1, amount, // 商户余额增加（1=收入）
                        "transfer_fail", transferNo, "转账失败退回"); // 业务类型、单号、备注
                    db.exec("UPDATE transfer_order SET state=-1,err_msg=? WHERE transfer_no=?", // 更新转账单状态为失败
                            {errMsg, transferNo}); // 错误信息、转账单号
                    transferState = -1; // 标记为失败
                }
            }
            // 插件不存在则保持 state=0，等管理员手动确认
        }
        // 无转账通道配置则保持 state=0，等管理员手动确认

        Json::Value data; // 响应数据
        data["transfer_no"]        = transferNo; // 转账单号
        data["amount"]             = ChannelService::fmtAmount(amount); // 转账金额
        data["real_amount"]        = ChannelService::fmtAmount(real); // 实际转账金额
        data["state"]              = transferState; // 转账状态
        data["channel_transfer_no"]= channelTransferNo; // 通道转账号
        if (!errMsg.empty()) data["err_msg"] = errMsg; // 如果有错误信息，添加到响应
        RESP_OK(cb, data); // 返回成功响应
    }

    // 转账记录方法（分页）
    void list(const drogon::HttpRequestPtr &req, // HTTP 请求对象
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        int page = pi(req->getParameter("page"), 1); // 获取页码（默认 1）
        int size = pi(req->getParameter("size"), 20); // 获取每页数量（默认 20）

        auto &db = PayDb::instance(); // 获取数据库单例
        auto cntR = db.query("SELECT COUNT(*) AS c FROM transfer_order WHERE mch_id=?", {mchId}); // 查询总数
        int total = cntR.empty() ? 0 : std::stoi(cntR[0]["c"]); // 获取总数
        auto rows = db.query( // 查询转账记录
            "SELECT * FROM transfer_order WHERE mch_id=? ORDER BY id DESC LIMIT ? OFFSET ?", // 按 ID 倒序，分页查询
            {mchId, std::to_string(size), std::to_string((page - 1) * size)}); // 商户 ID、分页参数

        Json::Value arr(Json::arrayValue); // 创建 JSON 数组
        for (auto &r : rows) { // 遍历每条转账记录
            Json::Value it; // 创建转账项
            for (auto &[k, v] : r) it[k] = v; // 将查询结果转换为 JSON
            it["id"] = std::stoi(r["id"]); // 将 ID 转为整数
            it["mch_id"] = std::stoi(r["mch_id"]); // 将商户 ID 转为整数
            it["account_type"] = std::stoi(r["account_type"]); // 将账户类型转为整数
            it["state"] = std::stoi(r["state"]); // 将状态转为整数
            it["created_at"] = (Json::Int64)std::stoll(r["created_at"]); // 将创建时间转为 Int64
            arr.append(it); // 添加到数组
        }
        Json::Value data; // 响应数据
        data["list"] = arr; // 转账记录列表
        data["total"] = total; // 总数
        RESP_OK(cb, data); // 返回成功响应
    }

    // 转账详情方法
    void detail(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        std::string no = req->getParameter("transfer_no"); // 获取转账单号参数
        auto r = PayDb::instance().queryOne( // 查询转账详情
            "SELECT * FROM transfer_order WHERE transfer_no=? AND mch_id=?", {no, mchId}); // 按转账单号和商户 ID 查询
        if (r.empty()) { RESP_ERR(cb, "转账单不存在"); return; } // 转账单不存在
        Json::Value data; // 响应数据
        for (auto &[k, v] : r) data[k] = v; // 将查询结果转换为 JSON
        data["state"] = std::stoi(r["state"]); // 将状态转为整数
        RESP_OK(cb, data); // 返回转账详情
    }

private:
    // 查找商户可用的转账通道(优先商户绑定的，其次系统默认)
    static PayDb::Row findTransferChannel(PayDb &db, const std::string &mchId, int accountType) { // 数据库、商户 ID、账户类型
        // 映射: 1=微信 → wxpay_transfer, 2=支付宝 → alipay_transfer, 3=银行卡 → bank_transfer
        std::string payType; // 支付类型
        if (accountType == 1)      payType = "wxpay_transfer"; // 微信转账
        else if (accountType == 2) payType = "alipay_transfer"; // 支付宝转账
        else                       payType = "bank_transfer"; // 银行卡转账

        // 商户绑定的转账通道
        auto rows = db.query( // 查询商户绑定的通道
            "SELECT c.id,c.plugin,c.params_json FROM pay_channel c " // 查询通道 ID、插件、参数
            "JOIN merchant_channel mc ON mc.channel_id=c.id " // 关联商户通道表
            "WHERE mc.mch_id=? AND c.pay_type=? AND c.state=1 AND mc.state=1 " // 按商户 ID、支付类型、状态查询
            "ORDER BY c.sort_order ASC LIMIT 1", // 按排序顺序取第一个
            {mchId, payType}); // 参数
        if (!rows.empty()) return rows[0]; // 如果找到，返回第一条

        // 回退到系统默认转账通道
        rows = db.query( // 查询系统默认通道
            "SELECT id,plugin,params_json FROM pay_channel " // 查询通道 ID、插件、参数
            "WHERE pay_type=? AND state=1 ORDER BY sort_order ASC LIMIT 1", // 按支付类型和状态查询
            {payType}); // 参数
        return rows.empty() ? PayDb::Row{} : rows[0]; // 返回第一条或空行
    }

    // 安全字符串转整数方法
    static int pi(const std::string &s, int def) { // 字符串和默认值
        try { return std::stoi(s); } catch(...) { return def; } // 尝试转换，失败返回默认值
    }
    // 生成转账单号方法
    static std::string generateTransferNo() { // 返回转账单号
        static std::mt19937 rng((unsigned)std::random_device{}()); // 初始化随机数生成器
        long long ts = std::time(nullptr); // 获取当前时间戳
        char buf[32]; // 缓冲区
        std::snprintf(buf, sizeof(buf), "T%lld%04d", ts, (int)(rng() % 10000)); // 格式：T + 时间戳 + 4位随机数
        return buf; // 返回转账单号
    }
};
