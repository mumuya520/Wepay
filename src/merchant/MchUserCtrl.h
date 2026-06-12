// WePay-Cpp — 商户端: 子账号管理
// 商户管理员可创建子账号并分配权限子集
// GET    /merchant/api/user/list      子账号列表
// POST   /merchant/api/user/add       创建子账号
// POST   /merchant/api/user/edit      编辑子账号
// POST   /merchant/api/user/state     启用/禁用子账号
// POST   /merchant/api/user/resetPwd  重置密码
// DELETE /merchant/api/user/del       删除子账号
// GET    /merchant/api/user/perms     权限列表
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // 时间库
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/PasswordUtils.h" // 密码工具
#include "../filters/MerchantAuthFilter.h" // 商户认证过滤器

// 商户子账号管理控制器类
class MchUserCtrl : public drogon::HttpController<MchUserCtrl> {
public:
    METHOD_LIST_BEGIN // 路由列表开始
        ADD_METHOD_TO(MchUserCtrl::list,     "/merchant/api/user/list",     drogon::Get,    "MerchantAuthFilter"); // 子账号列表
        ADD_METHOD_TO(MchUserCtrl::add,      "/merchant/api/user/add",      drogon::Post,   "MerchantAuthFilter"); // 创建子账号
        ADD_METHOD_TO(MchUserCtrl::edit,     "/merchant/api/user/edit",     drogon::Post,   "MerchantAuthFilter"); // 编辑子账号
        ADD_METHOD_TO(MchUserCtrl::state,    "/merchant/api/user/state",    drogon::Post,   "MerchantAuthFilter"); // 启用/禁用
        ADD_METHOD_TO(MchUserCtrl::resetPwd, "/merchant/api/user/resetPwd", drogon::Post,   "MerchantAuthFilter"); // 重置密码
        ADD_METHOD_TO(MchUserCtrl::del,      "/merchant/api/user/del",      drogon::Delete, "MerchantAuthFilter"); // 删除子账号
        ADD_METHOD_TO(MchUserCtrl::permList, "/merchant/api/user/perms",    drogon::Get,    "MerchantAuthFilter"); // 权限列表
    METHOD_LIST_END // 路由列表结束

    // 子账号列表方法
    void list(const drogon::HttpRequestPtr &req, // HTTP 请求对象
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        auto rows = PayDb::instance().query( // 查询子账号
            "SELECT id,username,real_name,phone,email,is_admin,permissions,state," // 查询子账号信息
            "last_login_at,created_at FROM mch_user WHERE mch_id=? ORDER BY id DESC", // 按 ID 倒序
            {mchId}); // 商户 ID 参数
        Json::Value arr(Json::arrayValue); // 创建 JSON 数组
        for (auto &r : rows) { // 遍历每个子账号
            Json::Value it; // 创建子账号项
            it["id"]            = std::stoi(r["id"]); // 子账号 ID
            it["username"]      = r["username"]; // 用户名
            it["real_name"]     = r["real_name"]; // 真实姓名
            it["phone"]         = r["phone"]; // 电话
            it["email"]         = r["email"]; // 邮箱
            it["is_admin"]      = std::stoi(r["is_admin"]); // 是否管理员
            it["state"]         = std::stoi(r["state"]); // 账号状态
            it["last_login_at"] = (Json::Int64)std::stoll(r["last_login_at"]); // 最后登录时间
            it["created_at"]    = (Json::Int64)std::stoll(r["created_at"]); // 创建时间
            // permissions 是 JSON 数组
            Json::Value perms; // 权限数组
            if (Json::Reader().parse(r["permissions"], perms)) it["permissions"] = perms; // 解析权限
            arr.append(it); // 添加到数组
        }
        RESP_OK(cb, arr); // 返回成功响应
    }

    // 创建子账号方法
    void add(const drogon::HttpRequestPtr &req, // HTTP 请求对象
             std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        auto body = req->getJsonObject(); // 获取 JSON 请求体
        if (!body) { RESP_ERR(cb, "格式错误"); return; } // 请求体不是 JSON
        auto &j = *body; // 引用 JSON 对象
        std::string username = j.get("username", "").asString(); // 获取用户名
        std::string password = j.get("password", "").asString(); // 获取密码
        if (username.empty() || password.size() < 6) { // 检查用户名和密码
            RESP_ERR(cb, "用户名必填，密码至少6位"); return; // 参数不完整
        }
        auto &db = PayDb::instance(); // 获取数据库单例
        auto exist = db.queryOne( // 查询用户名是否存在
            "SELECT id FROM mch_user WHERE mch_id=? AND username=?", {mchId, username}); // 按商户 ID 和用户名查询
        if (!exist.empty()) { RESP_ERR(cb, "该用户名已存在"); return; } // 用户名已存在

        std::string salt = PasswordUtils::generateSalt(); // 生成盐值
        std::string hash = PasswordUtils::hashPassword(password, salt); // 哈希密码

        // permissions 序列化为 JSON 数组
        Json::Value perms = j["permissions"]; // 获取权限数组
        if (!perms.isArray()) perms = Json::Value(Json::arrayValue); // 如果不是数组，创建空数组
        Json::StreamWriterBuilder wb; wb["indentation"] = ""; // 创建 JSON 写入器
        std::string permsStr = Json::writeString(wb, perms); // 序列化权限

        long long now = std::time(nullptr); // 获取当前时间戳
        db.exec("INSERT INTO mch_user(mch_id,username,password,salt,real_name,phone,email," // 插入子账号
                "is_admin,permissions,state,created_at) VALUES(?,?,?,?,?,?,?,?,?,1,?)", // 创建为启用状态
                {mchId, username, hash, salt, // 商户 ID、用户名、密码哈希、盐值
                 j.get("real_name", "").asString(), // 真实姓名
                 j.get("phone", "").asString(), // 电话
                 j.get("email", "").asString(), // 邮箱
                 std::to_string(j.get("is_admin", 0).asInt()), // 是否管理员
                 permsStr, std::to_string(now)}); // 权限、创建时间
        RESP_MSG(cb, "创建成功"); // 返回成功消息
    }

    // 编辑子账号方法
    void edit(const drogon::HttpRequestPtr &req, // HTTP 请求对象
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        auto body = req->getJsonObject(); // 获取 JSON 请求体
        if (!body) { RESP_ERR(cb, "格式错误"); return; } // 请求体不是 JSON
        auto &j = *body; // 引用 JSON 对象
        std::string id = j.get("id", "").asString(); // 获取子账号 ID

        Json::Value perms = j["permissions"]; // 获取权限数组
        if (!perms.isArray()) perms = Json::Value(Json::arrayValue); // 如果不是数组，创建空数组
        Json::StreamWriterBuilder wb; wb["indentation"] = ""; // 创建 JSON 写入器
        std::string permsStr = Json::writeString(wb, perms); // 序列化权限

        PayDb::instance().exec( // 更新子账号
            "UPDATE mch_user SET real_name=?,phone=?,email=?,is_admin=?,permissions=? " // 更新子账号信息
            "WHERE id=? AND mch_id=?", // 按 ID 和商户 ID 更新
            {j.get("real_name", "").asString(), // 真实姓名
             j.get("phone", "").asString(), // 电话
             j.get("email", "").asString(), // 邮箱
             std::to_string(j.get("is_admin", 0).asInt()), // 是否管理员
             permsStr, id, mchId}); // 权限、子账号 ID、商户 ID
        RESP_MSG(cb, "已更新"); // 返回更新成功消息
    }

    // 启用/禁用子账号方法
    void state(const drogon::HttpRequestPtr &req, // HTTP 请求对象
               std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        auto body = req->getJsonObject(); // 获取 JSON 请求体
        if (!body) { RESP_ERR(cb, "格式错误"); return; } // 请求体不是 JSON
        std::string id = (*body).get("id", "").asString(); // 获取子账号 ID
        int s = (*body).get("state", 0).asInt(); // 获取新状态
        PayDb::instance().exec("UPDATE mch_user SET state=? WHERE id=? AND mch_id=?", // 更新子账号状态
            {std::to_string(s), id, mchId}); // 状态、ID、商户 ID
        RESP_MSG(cb, "已更新"); // 返回更新成功消息
    }

    // 重置密码方法
    void resetPwd(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        auto body = req->getJsonObject(); // 获取 JSON 请求体
        if (!body) { RESP_ERR(cb, "格式错误"); return; } // 请求体不是 JSON
        std::string id = (*body).get("id", "").asString(); // 获取子账号 ID
        std::string pwd = (*body).get("password", "").asString(); // 获取新密码
        if (pwd.size() < 6) { RESP_ERR(cb, "密码至少6位"); return; } // 检查密码长度
        std::string salt = PasswordUtils::generateSalt(); // 生成盐值
        std::string hash = PasswordUtils::hashPassword(pwd, salt); // 哈希密码
        PayDb::instance().exec("UPDATE mch_user SET password=?,salt=? WHERE id=? AND mch_id=?", // 更新密码和盐值
            {hash, salt, id, mchId}); // 密码哈希、盐值、ID、商户 ID
        RESP_MSG(cb, "密码已重置"); // 返回重置成功消息
    }

    // 删除子账号方法
    void del(const drogon::HttpRequestPtr &req, // HTTP 请求对象
             std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        std::string id = req->getParameter("id"); // 从查询参数获取子账号 ID
        PayDb::instance().exec("DELETE FROM mch_user WHERE id=? AND mch_id=?", {id, mchId}); // 删除子账号
        RESP_MSG(cb, "已删除"); // 返回删除成功消息
    }

    // 权限列表方法（预定义权限码）
    void permList(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        Json::Value arr(Json::arrayValue); // 创建权限数组
        auto add = [&arr](const std::string &code, const std::string &name) { // 添加权限的 lambda 函数
            Json::Value it; it["code"] = code; it["name"] = name; arr.append(it); // 创建权限项并添加
        };
        add("order:view",     "查看订单"); // 订单查看权限
        add("order:refund",   "申请退款"); // 退款权限
        add("transfer:view",  "查看转账"); // 转账查看权限
        add("transfer:create","发起转账"); // 转账发起权限
        add("settle:view",    "查看结算"); // 结算查看权限
        add("settle:apply",   "申请结算"); // 结算申请权限
        add("user:manage",    "子账号管理"); // 子账号管理权限
        add("config:edit",    "修改账户配置"); // 账户配置修改权限
        RESP_OK(cb, arr); // 返回权限列表
    }
};
