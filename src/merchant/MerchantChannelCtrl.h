// WePay-Cpp — 商户支付通道管理控制器
// 负责：获取可用通道、绑定通道、解绑通道、修改费率、启用禁用通道
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <json/json.h> // JSON 库
#include "../common/PayDb.h" // 数据库操作
#include "../filters/MerchantAuthFilter.h" // 商户认证过滤器

using namespace drogon; // Drogon 命名空间

// 商户支付通道管理控制器类
class MerchantChannelCtrl : public drogon::HttpController<MerchantChannelCtrl> {
public:
    METHOD_LIST_BEGIN // 路由列表开始
        // 获取所有可用的支付通道（管理员启用的）
        ADD_METHOD_TO(MerchantChannelCtrl::listAvailableChannels,
                      "/merchant/api/channels/available", drogon::Get, "MerchantAuthFilter");
        
        // 获取商户已绑定的支付通道
        ADD_METHOD_TO(MerchantChannelCtrl::listBoundChannels,
                      "/merchant/api/channels/bound", drogon::Get, "MerchantAuthFilter");
        
        // 为商户绑定支付通道
        ADD_METHOD_TO(MerchantChannelCtrl::bindChannel,
                      "/merchant/api/channels/bind", drogon::Post, "MerchantAuthFilter");
        
        // 为商户解绑支付通道
        ADD_METHOD_TO(MerchantChannelCtrl::unbindChannel,
                      "/merchant/api/channels/unbind", drogon::Post, "MerchantAuthFilter");
        
        // 修改商户的通道费率
        ADD_METHOD_TO(MerchantChannelCtrl::updateChannelRate,
                      "/merchant/api/channels/rate", drogon::Post, "MerchantAuthFilter");
        
        // 关闭/启用商户的支付通道
        ADD_METHOD_TO(MerchantChannelCtrl::toggleChannelState,
                      "/merchant/api/channels/toggle", drogon::Post, "MerchantAuthFilter");
    METHOD_LIST_END // 路由列表结束

private:
    // ══════════════════════════════════════════════════════════════════
    // 获取所有可用的支付通道（管理员启用的）
    // ══════════════════════════════════════════════════════════════════
    // 获取所有可用支付通道方法
    void listAvailableChannels(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                               std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        auto &db = PayDb::instance(); // 获取数据库单例
        
        // 查询所有启用的支付通道（只显示管理员启用的）
        // state=1 表示通道已启用，商户可以绑定
        auto channels = db.query( // 查询支付通道
            "SELECT id, channel_code, channel_name, pay_type, rate, state, " // 查询通道基本信息
            "       display_name, description, sort_order " // 查询显示信息和排序
            "FROM pay_channel " // 从支付通道表
            "WHERE state=1 " // 仅查询启用的通道
            "ORDER BY sort_order ASC, pay_type ASC", // 按排序和支付类型排序
            {}); // 无参数
        
        Json::Value data; // 响应数据
        data["total"] = (Json::Int)channels.size(); // 通道总数
        data["msg"] = "这些是管理员启用的支付通道，商户可以选择绑定"; // 说明信息
        
        Json::Value items(Json::arrayValue); // 创建通道数组
        for (auto &ch : channels) { // 遍历每个通道
            Json::Value item; // 创建通道项
            item["id"] = std::stoi(ch["id"]); // 通道 ID
            item["channel_code"] = ch["channel_code"]; // 通道代码
            item["channel_name"] = ch["channel_name"]; // 通道名称
            item["pay_type"] = ch["pay_type"]; // 支付类型
            item["rate"] = ch["rate"]; // 费率
            item["display_name"] = ch.count("display_name") ? ch["display_name"] : ""; // 显示名称
            item["description"] = ch.count("description") ? ch["description"] : ""; // 描述
            item["enabled"] = true;  // 标记为启用
            items.append(item); // 添加到数组
        }
        data["items"] = items; // 返回通道列表
        
        auto resp = drogon::HttpResponse::newHttpJsonResponse(data); // 创建 JSON 响应
        cb(resp); // 回调响应
    }

    // ══════════════════════════════════════════════════════════════════
    // 获取商户已绑定的支付通道
    // ══════════════════════════════════════════════════════════════════
    // 获取商户已绑定通道方法
    void listBoundChannels(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                           std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        auto &db = PayDb::instance(); // 获取数据库单例
        
        // 从 JWT token 中提取商户 ID
        std::string authHeader = req->getHeader("Authorization"); // 获取授权头
        if (authHeader.empty()) { // 如果授权头为空
            Json::Value err; // 创建错误响应
            err["code"] = -1; // 错误代码
            err["msg"] = "未授权"; // 错误信息
            auto resp = drogon::HttpResponse::newHttpJsonResponse(err); // 创建 JSON 响应
            resp->setStatusCode(drogon::k401Unauthorized); // 设置 401 状态码
            cb(resp); // 回调响应
            return; // 返回
        }
        
        // 简单解析 token 中的商户 ID（格式: "mch:ID:username"）
        // 实际应该用 JWT 库解析，这里简化处理
        std::string token = authHeader; // 获取 token
        if (token.substr(0, 7) == "Bearer ") { // 如果有 Bearer 前缀
            token = token.substr(7); // 移除前缀
        }
        
        // 从数据库查询商户 ID（通过 token 的 sub 字段）
        // 这里简化：直接从请求头或 session 中获取
        // 实际应该通过 MerchantAuthFilter 传递
        std::string mchIdStr = req->getHeader("X-Merchant-Id"); // 从请求头获取商户 ID
        if (mchIdStr.empty()) { // 如果商户 ID 为空
            Json::Value err; // 创建错误响应
            err["code"] = -1; // 错误代码
            err["msg"] = "无法获取商户信息"; // 错误信息
            auto resp = drogon::HttpResponse::newHttpJsonResponse(err); // 创建 JSON 响应
            resp->setStatusCode(drogon::k400BadRequest); // 设置 400 状态码
            cb(resp); // 回调响应
            return; // 返回
        }
        
        int mchId = std::stoi(mchIdStr); // 转换商户 ID 为整数
        
        // 查询商户已绑定的通道
        auto channels = db.query( // 查询商户绑定的通道
            "SELECT mc.id, c.id as channel_id, c.channel_code, c.channel_name, " // 查询通道基本信息
            "       c.pay_type, COALESCE(mc.rate, c.rate) as rate, mc.state " // 查询费率和状态
            "FROM merchant_channel mc " // 从商户通道表
            "JOIN pay_channel c ON mc.channel_id = c.id " // 关联支付通道表
            "WHERE mc.mch_id = ? AND mc.state = 1 " // 按商户 ID 和状态查询
            "ORDER BY c.pay_type, c.sort_order ASC", // 按支付类型和排序排序
            {std::to_string(mchId)}); // 商户 ID 参数
        
        Json::Value data; // 响应数据
        data["total"] = (Json::Int)channels.size(); // 通道总数
        
        Json::Value items(Json::arrayValue); // 创建通道数组
        for (auto &ch : channels) { // 遍历每个通道
            Json::Value item; // 创建通道项
            item["id"] = std::stoi(ch["id"]); // 商户通道关系 ID
            item["channel_id"] = std::stoi(ch["channel_id"]); // 通道 ID
            item["channel_code"] = ch["channel_code"]; // 通道代码
            item["channel_name"] = ch["channel_name"]; // 通道名称
            item["pay_type"] = ch["pay_type"]; // 支付类型
            item["rate"] = ch["rate"]; // 费率
            items.append(item); // 添加到数组
        }
        data["items"] = items; // 返回通道列表
        
        auto resp = drogon::HttpResponse::newHttpJsonResponse(data); // 创建 JSON 响应
        cb(resp); // 回调响应
    }

    // ══════════════════════════════════════════════════════════════════
    // 为商户绑定支付通道
    // ══════════════════════════════════════════════════════════════════
    // 绑定支付通道方法
    void bindChannel(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        auto body = req->getJsonObject(); // 获取 JSON 请求体
        if (!body) { // 如果请求体不是 JSON
            Json::Value err; // 创建错误响应
            err["code"] = -1; // 错误代码
            err["msg"] = "请求体格式错误"; // 错误信息
            auto resp = drogon::HttpResponse::newHttpJsonResponse(err); // 创建 JSON 响应
            cb(resp); // 回调响应
            return; // 返回
        }
        
        auto &j = *body; // 引用 JSON 对象
        int channelId = j.get("channel_id", 0).asInt(); // 获取通道 ID
        
        if (channelId <= 0) { // 如果通道 ID 无效
            Json::Value err; // 创建错误响应
            err["code"] = -1; // 错误代码
            err["msg"] = "通道ID不能为空"; // 错误信息
            auto resp = drogon::HttpResponse::newHttpJsonResponse(err); // 创建 JSON 响应
            cb(resp); // 回调响应
            return; // 返回
        }
        
        auto &db = PayDb::instance(); // 获取数据库单例
        
        // 从请求头获取商户 ID
        std::string mchIdStr = req->getHeader("X-Merchant-Id"); // 从请求头获取商户 ID
        if (mchIdStr.empty()) { // 如果商户 ID 为空
            Json::Value err; // 创建错误响应
            err["code"] = -1; // 错误代码
            err["msg"] = "无法获取商户信息"; // 错误信息
            auto resp = drogon::HttpResponse::newHttpJsonResponse(err); // 创建 JSON 响应
            cb(resp); // 回调响应
            return; // 返回
        }
        
        int mchId = std::stoi(mchIdStr); // 转换商户 ID 为整数
        
        // 检查通道是否存在且被管理员启用
        // 只有管理员启用的通道（state=1）才能被商户绑定
        auto channel = db.queryOne( // 查询通道
            "SELECT id, channel_code, channel_name FROM pay_channel WHERE id=? AND state=1", // 查询启用的通道
            {std::to_string(channelId)}); // 通道 ID 参数
        if (channel.empty()) { // 如果通道不存在或已禁用
            Json::Value err; // 创建错误响应
            err["code"] = -1; // 错误代码
            err["msg"] = "支付通道不存在或已被管理员禁用，请联系管理员启用该通道"; // 错误信息
            auto resp = drogon::HttpResponse::newHttpJsonResponse(err); // 创建 JSON 响应
            cb(resp); // 回调响应
            return; // 返回
        }
        
        // 检查是否已绑定
        auto existing = db.queryOne( // 查询已绑定的通道
            "SELECT id FROM merchant_channel WHERE mch_id=? AND channel_id=?", // 查询商户通道关系
            {std::to_string(mchId), std::to_string(channelId)}); // 商户 ID 和通道 ID 参数
        
        if (!existing.empty()) { // 如果已绑定
            Json::Value err; // 创建错误响应
            err["code"] = -1; // 错误代码
            err["msg"] = "该通道已绑定"; // 错误信息
            auto resp = drogon::HttpResponse::newHttpJsonResponse(err); // 创建 JSON 响应
            cb(resp); // 回调响应
            return; // 返回
        }
        
        // 绑定通道
        db.exec("INSERT INTO merchant_channel(mch_id, channel_id, rate, state) " // 插入商户通道关系
                "VALUES(?, ?, ?, 1)", // 状态为 1（启用）
                {std::to_string(mchId), std::to_string(channelId), ""}); // 商户 ID、通道 ID、费率为空
        
        Json::Value data; // 响应数据
        data["code"] = 1; // 成功代码
        data["msg"] = "绑定成功"; // 成功信息
        auto resp = drogon::HttpResponse::newHttpJsonResponse(data); // 创建 JSON 响应
        cb(resp); // 回调响应
    }

    // ══════════════════════════════════════════════════════════════════
    // 为商户解绑支付通道
    // ══════════════════════════════════════════════════════════════════
    // 解绑支付通道方法
    void unbindChannel(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                       std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        auto body = req->getJsonObject(); // 获取 JSON 请求体
        if (!body) { // 如果请求体不是 JSON
            Json::Value err; // 创建错误响应
            err["code"] = -1; // 错误代码
            err["msg"] = "请求体格式错误"; // 错误信息
            auto resp = drogon::HttpResponse::newHttpJsonResponse(err); // 创建 JSON 响应
            cb(resp); // 回调响应
            return; // 返回
        }
        
        auto &j = *body; // 引用 JSON 对象
        int channelId = j.get("channel_id", 0).asInt(); // 获取通道 ID
        
        if (channelId <= 0) { // 如果通道 ID 无效
            Json::Value err; // 创建错误响应
            err["code"] = -1; // 错误代码
            err["msg"] = "通道ID不能为空"; // 错误信息
            auto resp = drogon::HttpResponse::newHttpJsonResponse(err); // 创建 JSON 响应
            cb(resp); // 回调响应
            return; // 返回
        }
        
        auto &db = PayDb::instance(); // 获取数据库单例
        
        // 从请求头获取商户 ID
        std::string mchIdStr = req->getHeader("X-Merchant-Id"); // 从请求头获取商户 ID
        if (mchIdStr.empty()) { // 如果商户 ID 为空
            Json::Value err; // 创建错误响应
            err["code"] = -1; // 错误代码
            err["msg"] = "无法获取商户信息"; // 错误信息
            auto resp = drogon::HttpResponse::newHttpJsonResponse(err); // 创建 JSON 响应
            cb(resp); // 回调响应
            return; // 返回
        }
        
        int mchId = std::stoi(mchIdStr); // 转换商户 ID 为整数
        
        // 解绑通道（软删除）
        db.exec("UPDATE merchant_channel SET state=0 WHERE mch_id=? AND channel_id=?", // 更新通道状态为 0（禁用）
                {std::to_string(mchId), std::to_string(channelId)}); // 商户 ID 和通道 ID 参数
        
        Json::Value data; // 响应数据
        data["code"] = 1; // 成功代码
        data["msg"] = "解绑成功"; // 成功信息
        auto resp = drogon::HttpResponse::newHttpJsonResponse(data); // 创建 JSON 响应
        cb(resp); // 回调响应
    }

    // ══════════════════════════════════════════════════════════════════
    // 修改商户的通道费率
    // ══════════════════════════════════════════════════════════════════
    // 更新通道费率方法
    void updateChannelRate(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                           std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        auto body = req->getJsonObject(); // 获取 JSON 请求体
        if (!body) { // 如果请求体不是 JSON
            Json::Value err; // 创建错误响应
            err["code"] = -1; // 错误代码
            err["msg"] = "请求体格式错误"; // 错误信息
            auto resp = drogon::HttpResponse::newHttpJsonResponse(err); // 创建 JSON 响应
            cb(resp); // 回调响应
            return; // 返回
        }
        
        auto &j = *body; // 引用 JSON 对象
        int channelId = j.get("channel_id", 0).asInt(); // 获取通道 ID
        std::string rate = j.get("rate", "").asString(); // 获取费率
        
        if (channelId <= 0 || rate.empty()) { // 如果通道 ID 或费率无效
            Json::Value err; // 创建错误响应
            err["code"] = -1; // 错误代码
            err["msg"] = "通道ID和费率不能为空"; // 错误信息
            auto resp = drogon::HttpResponse::newHttpJsonResponse(err); // 创建 JSON 响应
            cb(resp); // 回调响应
            return; // 返回
        }
        
        auto &db = PayDb::instance(); // 获取数据库单例
        
        // 从请求头获取商户 ID
        std::string mchIdStr = req->getHeader("X-Merchant-Id"); // 从请求头获取商户 ID
        if (mchIdStr.empty()) { // 如果商户 ID 为空
            Json::Value err; // 创建错误响应
            err["code"] = -1; // 错误代码
            err["msg"] = "无法获取商户信息"; // 错误信息
            auto resp = drogon::HttpResponse::newHttpJsonResponse(err); // 创建 JSON 响应
            cb(resp); // 回调响应
            return; // 返回
        }
        
        int mchId = std::stoi(mchIdStr); // 转换商户 ID 为整数
        
        // 更新费率
        db.exec("UPDATE merchant_channel SET rate=? WHERE mch_id=? AND channel_id=?", // 更新商户通道费率
                {rate, std::to_string(mchId), std::to_string(channelId)}); // 费率、商户 ID、通道 ID 参数
        
        Json::Value data; // 响应数据
        data["code"] = 1; // 成功代码
        data["msg"] = "费率更新成功"; // 成功信息
        auto resp = drogon::HttpResponse::newHttpJsonResponse(data); // 创建 JSON 响应
        cb(resp); // 回调响应
    }

    // ══════════════════════════════════════════════════════════════════
    // 关闭/启用商户的支付通道
    // ══════════════════════════════════════════════════════════════════
    // 切换通道状态方法
    void toggleChannelState(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                            std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        auto body = req->getJsonObject(); // 获取 JSON 请求体
        if (!body) { // 如果请求体不是 JSON
            Json::Value err; // 创建错误响应
            err["code"] = -1; // 错误代码
            err["msg"] = "请求体格式错误"; // 错误信息
            auto resp = drogon::HttpResponse::newHttpJsonResponse(err); // 创建 JSON 响应
            cb(resp); // 回调响应
            return; // 返回
        }
        
        auto &j = *body; // 引用 JSON 对象
        int channelId = j.get("channel_id", 0).asInt(); // 获取通道 ID
        int state = j.get("state", -1).asInt();  // 获取状态（1=启用, 0=禁用）
        
        if (channelId <= 0 || (state != 0 && state != 1)) { // 如果通道 ID 或状态无效
            Json::Value err; // 创建错误响应
            err["code"] = -1; // 错误代码
            err["msg"] = "通道ID和状态不能为空，状态值为0（禁用）或1（启用)"; // 错误信息
            auto resp = drogon::HttpResponse::newHttpJsonResponse(err); // 创建 JSON 响应
            cb(resp); // 回调响应
            return; // 返回
        }
        
        auto &db = PayDb::instance(); // 获取数据库单例
        
        // 从请求头获取商户 ID
        std::string mchIdStr = req->getHeader("X-Merchant-Id"); // 从请求头获取商户 ID
        if (mchIdStr.empty()) { // 如果商户 ID 为空
            Json::Value err; // 创建错误响应
            err["code"] = -1; // 错误代码
            err["msg"] = "无法获取商户信息"; // 错误信息
            auto resp = drogon::HttpResponse::newHttpJsonResponse(err); // 创建 JSON 响应
            cb(resp); // 回调响应
            return; // 返回
        }
        
        int mchId = std::stoi(mchIdStr); // 转换商户 ID 为整数
        
        // 检查通道是否存在且已绑定
        auto existing = db.queryOne( // 查询已绑定的通道
            "SELECT id FROM merchant_channel WHERE mch_id=? AND channel_id=?", // 查询商户通道关系
            {std::to_string(mchId), std::to_string(channelId)}); // 商户 ID 和通道 ID 参数
        
        if (existing.empty()) { // 如果通道未绑定或不存在
            Json::Value err; // 创建错误响应
            err["code"] = -1; // 错误代码
            err["msg"] = "该通道未绑定或不存在"; // 错误信息
            auto resp = drogon::HttpResponse::newHttpJsonResponse(err); // 创建 JSON 响应
            cb(resp); // 回调响应
            return; // 返回
        }
        
        // 更新通道状态
        db.exec("UPDATE merchant_channel SET state=? WHERE mch_id=? AND channel_id=?", // 更新商户通道状态
                {std::to_string(state), std::to_string(mchId), std::to_string(channelId)}); // 状态、商户 ID、通道 ID 参数
        
        Json::Value data; // 响应数据
        data["code"] = 1; // 成功代码
        std::string stateMsg = (state == 1) ? "启用" : "禁用"; // 状态信息
        data["msg"] = "通道已" + stateMsg; // 成功信息
        data["state"] = state; // 返回新状态
        auto resp = drogon::HttpResponse::newHttpJsonResponse(data); // 创建 JSON 响应
        cb(resp); // 回调响应
    }
};
