// WePay-Cpp — 商户端补充控制器
// 1. PayTest 商户支付测试
// 2. PayWay 商户查询启用的支付方式
// 3. SysArticle 商户读取系统公告
// 4. MchPayPassage 商户自助查看绑定通道
// 5. MchConfig 商户自定义 KV 配置
//
// === 支付测试 ===
// POST /merchant/api/payTest/create     发起测试支付订单
// POST /merchant/api/payTest/notify     模拟回调到商户测试地址
//
// === 支付方式 ===
// GET  /merchant/api/payWay/list        启用的支付方式列表
//
// === 公告 ===
// GET  /merchant/api/article/list       商户可见公告
//
// === 商户视角的通道与配置 ===
// GET  /merchant/api/myChannels         我已绑定的通道
// GET  /merchant/api/myConfig/list      我的扩展配置
// POST /merchant/api/myConfig/save      保存扩展配置
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // 时间库
#include <random> // 随机数生成
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/HttpCaller.h" // HTTP 调用工具
#include "../common/Md5Utils.h" // MD5 和签名工具
#include "../filters/MerchantAuthFilter.h" // 商户认证过滤器

// 商户补充控制器类
class MerchantExtraCtrl : public drogon::HttpController<MerchantExtraCtrl> {
public:
    METHOD_LIST_BEGIN // 路由列表开始
        ADD_METHOD_TO(MerchantExtraCtrl::testCreate, "/merchant/api/payTest/create", drogon::Post, "MerchantAuthFilter"); // 创建测试订单
        ADD_METHOD_TO(MerchantExtraCtrl::testNotify, "/merchant/api/payTest/notify", drogon::Post, "MerchantAuthFilter"); // 模拟回调
        ADD_METHOD_TO(MerchantExtraCtrl::payWayList, "/merchant/api/payWay/list",    drogon::Get,  "MerchantAuthFilter"); // 支付方式列表
        ADD_METHOD_TO(MerchantExtraCtrl::articleList,"/merchant/api/article/list",   drogon::Get,  "MerchantAuthFilter"); // 公告列表
        ADD_METHOD_TO(MerchantExtraCtrl::myChannels, "/merchant/api/myChannels",     drogon::Get,  "MerchantAuthFilter"); // 我的通道
        ADD_METHOD_TO(MerchantExtraCtrl::cfgList,    "/merchant/api/myConfig/list",  drogon::Get,  "MerchantAuthFilter"); // 配置列表
        ADD_METHOD_TO(MerchantExtraCtrl::cfgSave,    "/merchant/api/myConfig/save",  drogon::Post, "MerchantAuthFilter"); // 保存配置
    METHOD_LIST_END // 路由列表结束

    // 创建测试订单方法（模拟商户调用 /gateway/create）
    void testCreate(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        auto body = req->getJsonObject(); // 获取 JSON 请求体
        if (!body) { RESP_ERR(cb, "格式错误"); return; } // 请求体不是 JSON

        auto &db = PayDb::instance(); // 获取数据库单例
        auto mch = db.queryOne( // 查询商户信息
            "SELECT mch_no,mch_key FROM merchant WHERE id=?", {mchId}); // 按 ID 查询
        if (mch.empty()) { RESP_ERR(cb, "商户不存在"); return; } // 商户不存在

        // 生成测试订单号 + 假签名 + 提示 URL
        char outNo[32]; // 订单号缓冲区
        std::snprintf(outNo, sizeof(outNo), "TEST%lld", (long long)std::time(nullptr)); // 生成测试订单号
        std::string payType = (*body).get("pay_type", "wxpay").asString(); // 获取支付类型
        std::string amount  = (*body).get("amount", "0.01").asString(); // 获取金额
        std::string subject = (*body).get("subject", "支付测试").asString(); // 获取主题

        // 拼接签名 (与 EpaySign::sign 一致: 字典序 k=v&k=v 直接拼 key)
        std::map<std::string, std::string> p = { // 签名参数
            {"amount", amount}, {"mch_id", mch["mch_no"]}, // 金额、商户号
            {"out_trade_no", outNo}, {"pay_type", payType}, // 订单号、支付类型
            {"subject", subject} // 主题
        };
        std::string sign = EpaySign::sign(p, mch["mch_key"]); // 生成签名

        Json::Value data; // 响应数据
        data["test_url"] = "/gateway/create"; // 测试 URL
        data["mch_id"]   = mch["mch_no"]; // 商户号
        data["out_trade_no"] = std::string(outNo); // 订单号
        data["pay_type"] = payType; // 支付类型
        data["amount"]   = amount; // 金额
        data["subject"]  = subject; // 主题
        data["sign"]     = sign; // 签名
        data["hint"]     = "请将以上参数 POST 到 test_url 测试下单"; // 提示信息
        RESP_OK(cb, data); // 返回成功响应
    }

    // 模拟回调方法
    void testNotify(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        auto body = req->getJsonObject(); // 获取 JSON 请求体
        if (!body) { RESP_ERR(cb, "格式错误"); return; } // 请求体不是 JSON
        std::string url = (*body).get("notify_url", "").asString(); // 获取回调 URL
        std::string params = (*body).get("params", "").asString(); // 获取参数
        if (url.empty()) { RESP_ERR(cb, "notify_url 必填"); return; } // URL 为空

        // 异步打到商户测试地址
        HttpCaller::asyncPost(url, params, // 异步 POST 请求
            "application/x-www-form-urlencoded", // 内容类型
            [](bool ok, int code, const std::string &resp) { // 回调处理
                std::cout << "[PayTest] notify result: ok=" << ok // 打印结果
                          << " code=" << code << " body=" << resp.substr(0, 200) << std::endl; // 打印响应
            });
        RESP_MSG(cb, "已发起测试通知，请观察控制台日志"); // 返回成功消息
    }

    // 支付方式列表方法
    void payWayList(const drogon::HttpRequestPtr &, // HTTP 请求对象（未使用）
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        auto rows = PayDb::instance().query( // 查询支付方式
            "SELECT way_code,way_name,icon,bg_color,if_code FROM pay_way " // 查询支付方式信息
            "WHERE state=1 ORDER BY sort_order,id", {}); // 仅查询启用的方式
        Json::Value arr(Json::arrayValue); // 创建 JSON 数组
        for (auto &r : rows) { // 遍历每个支付方式
            Json::Value it; // 创建支付方式项
            for (auto &[k, v] : r) it[k] = v; // 复制所有字段
            arr.append(it); // 添加到数组
        }
        RESP_OK(cb, arr); // 返回成功响应
    }

    // 公告列表方法
    void articleList(const drogon::HttpRequestPtr &, // HTTP 请求对象（未使用）
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        auto rows = PayDb::instance().query( // 查询公告
            "SELECT id,title,article_type,is_top,publish_at FROM sys_article " // 查询公告信息
            "WHERE state=1 AND (target='all' OR target='merchant') " // 仅查询启用的商户公告
            "ORDER BY is_top DESC, id DESC LIMIT 20", {}); // 按置顶和 ID 倒序，限制 20 条
        Json::Value arr(Json::arrayValue); // 创建 JSON 数组
        for (auto &r : rows) { // 遍历每个公告
            Json::Value it; // 创建公告项
            it["id"] = std::stoi(r["id"]); // 公告 ID
            it["title"] = r["title"]; // 公告标题
            it["article_type"] = std::stoi(r["article_type"]); // 公告类型
            it["is_top"] = std::stoi(r["is_top"]); // 是否置顶
            it["publish_at"] = (Json::Int64)std::stoll(r["publish_at"]); // 发布时间
            arr.append(it); // 添加到数组
        }
        RESP_OK(cb, arr); // 返回成功响应
    }

    // 我的通道方法
    void myChannels(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        auto rows = PayDb::instance().query( // 查询商户绑定的通道
            "SELECT c.id,c.channel_code,c.channel_name,c.pay_type,c.plugin,c.state," // 查询通道信息
            "mc.rate AS bind_rate FROM pay_channel c " // 查询绑定费率
            "INNER JOIN merchant_channel mc ON mc.channel_id=c.id " // 关联商户通道表
            "WHERE mc.mch_id=? AND c.state=1 ORDER BY c.sort_order", {mchId}); // 按商户 ID 和排序查询
        Json::Value arr(Json::arrayValue); // 创建 JSON 数组
        for (auto &r : rows) { // 遍历每个通道
            Json::Value it; // 创建通道项
            for (auto &[k, v] : r) it[k] = v; // 复制所有字段
            it["id"] = std::stoi(r["id"]); // 通道 ID
            it["state"] = std::stoi(r["state"]); // 通道状态
            arr.append(it); // 添加到数组
        }
        RESP_OK(cb, arr); // 返回成功响应
    }

    // 配置列表方法
    void cfgList(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        auto rows = PayDb::instance().query( // 查询商户配置
            "SELECT id,config_key,config_value,remark,updated_at FROM mch_config " // 查询配置信息
            "WHERE mch_id=? ORDER BY config_key", {mchId}); // 按商户 ID 和键排序
        Json::Value arr(Json::arrayValue); // 创建 JSON 数组
        for (auto &r : rows) { // 遍历每个配置
            Json::Value it; // 创建配置项
            it["id"] = std::stoi(r["id"]); // 配置 ID
            it["config_key"] = r["config_key"]; // 配置键
            it["config_value"] = r["config_value"]; // 配置值
            it["remark"] = r["remark"]; // 备注
            it["updated_at"] = (Json::Int64)std::stoll(r["updated_at"]); // 更新时间
            arr.append(it); // 添加到数组
        }
        RESP_OK(cb, arr); // 返回成功响应
    }

    // 保存配置方法
    void cfgSave(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                 std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        auto body = req->getJsonObject(); // 获取 JSON 请求体
        if (!body) { RESP_ERR(cb, "格式错误"); return; } // 请求体不是 JSON
        auto &j = *body; // 引用 JSON 对象
        PayDb::instance().exec( // 插入或更新配置
            "INSERT OR REPLACE INTO mch_config(mch_id,config_key,config_value,remark,updated_at) " // 插入或替换配置
            "VALUES(?,?,?,?,?)", // 参数占位符
            {mchId, // 商户 ID
             j.get("config_key", "").asString(), // 配置键
             j.get("config_value", "").asString(), // 配置值
             j.get("remark", "").asString(), // 备注
             std::to_string(std::time(nullptr))}); // 更新时间
        RESP_MSG(cb, "已保存"); // 返回成功消息
    }
};
