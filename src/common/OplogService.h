// WePay-Cpp — 操作日志服务
// 记录管理员/商户的关键操作到 oplog 表
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <ctime> // C 时间库
#include "PayDb.h" // 数据库操作

// 操作日志服务类
// 记录管理员和商户的关键操作到 oplog 表
class OplogService {
public:
    // ── 操作模块常量 ──────────────────────────────────
    // 商户管理模块
    static constexpr const char* MOD_MERCHANT  = "merchant";
    // 支付通道模块
    static constexpr const char* MOD_CHANNEL   = "channel";
    // 订单模块
    static constexpr const char* MOD_ORDER     = "order";
    // 结算模块
    static constexpr const char* MOD_SETTLE    = "settle";
    // 配置管理模块
    static constexpr const char* MOD_CONFIG    = "config";
    // 设备管理模块
    static constexpr const char* MOD_DEVICE    = "device";
    // 认证模块
    static constexpr const char* MOD_AUTH      = "auth";

    // 记录操作日志（通用方法）
    // 参数 operatorType：操作者类型（1=管理员, 2=商户）
    // 参数 operatorId：操作者 ID（用户名或商户 ID）
    // 参数 module：操作模块（如 "merchant"、"channel" 等）
    // 参数 action：操作动作（如 "create"、"update"、"delete" 等）
    // 参数 targetId：目标对象 ID（如被操作的商户 ID、订单 ID 等）
    // 参数 detail：操作详情（如修改的字段内容）
    // 参数 ip：操作者 IP 地址
    static void log(int operatorType, const std::string &operatorId,
                    const std::string &module, const std::string &action,
                    const std::string &targetId = "",
                    const std::string &detail = "",
                    const std::string &ip = "") {
        // 获取当前时间戳
        long long now = std::time(nullptr);
        // 将模块和动作合并为 "module:action" 格式
        // 字段映射说明：
        // - operator_type → user_type（操作者类型）
        // - operator_id → username（操作者用户名）
        // - module + action → action（合并后的动作）
        // - targetId → target（目标对象 ID）
        std::string combinedAction = module.empty() ? action : (module + ":" + action);
        // 插入操作日志到数据库
        PayDb::instance().exec(
            // SQL 语句：插入操作日志
            "INSERT INTO oplog(user_type,user_id,username,action,target,detail,ip,created_at) "
            "VALUES(?,0,?,?,?,?,?,?)",
            // 参数：操作者类型、操作者 ID、操作者用户名、合并后的动作、
            //      目标对象 ID、操作详情、IP 地址、创建时间
            {std::to_string(operatorType), operatorId, combinedAction,
             targetId, detail, ip, std::to_string(now)});
    }

    // 记录管理员操作日志
    // 从 HTTP 请求头自动获取管理员信息和 IP 地址
    // 参数 req：HTTP 请求对象
    // 参数 module：操作模块
    // 参数 action：操作动作
    // 参数 targetId：目标对象 ID
    // 参数 detail：操作详情
    static void adminLog(const drogon::HttpRequestPtr &req,
                         const std::string &module,
                         const std::string &action,
                         const std::string &targetId = "",
                         const std::string &detail = "") {
        // 获取请求者的 IP 地址
        std::string ip = req->getPeerAddr().toIp();
        // 从 JWT 解析出的管理员用户名（AdminAuthFilter 验证后）
        // 注：实际应从 JWT token 中提取，这里简化为 "admin"
        std::string admin = "admin";
        // 调用通用日志记录方法（operatorType=1 表示管理员）
        log(1, admin, module, action, targetId, detail, ip);
    }

    // 记录商户操作日志
    // 从 HTTP 请求头自动获取商户信息和 IP 地址
    // 参数 req：HTTP 请求对象
    // 参数 module：操作模块
    // 参数 action：操作动作
    // 参数 targetId：目标对象 ID
    // 参数 detail：操作详情
    static void mchLog(const drogon::HttpRequestPtr &req,
                       const std::string &module,
                       const std::string &action,
                       const std::string &targetId = "",
                       const std::string &detail = "") {
        // 从请求头中获取商户 ID
        std::string mchId = req->getHeader("X-Mch-Id");
        // 获取请求者的 IP 地址
        std::string ip = req->getPeerAddr().toIp();
        // 调用通用日志记录方法（operatorType=2 表示商户）
        log(2, mchId, module, action, targetId, detail, ip);
    }

    // 查询操作日志（分页）
    // 参数 page：页码（从 1 开始）
    // 参数 size：每页记录数（最多 100 条）
    // 参数 module：操作模块过滤（可选）
    // 参数 operatorId：操作者 ID 过滤（可选）
    // 返回：JSON 格式的日志数据（包含 list 和 total）
    static Json::Value queryLogs(int page, int size,
                                  const std::string &module = "",
                                  const std::string &operatorId = "") {
        // 获取数据库实例
        auto &db = PayDb::instance();
        // 验证页码（最小为 1）
        if (page < 1)
            page = 1;
        // 验证每页记录数（范围 1-100，默认 20）
        if (size < 1 || size > 100)
            size = 20;
        // 计算数据库查询的偏移量
        int offset = (page - 1) * size;

        // 构建 WHERE 条件
        std::string where = "1=1";
        // 查询参数列表
        std::vector<std::string> params;
        // 如果指定了模块，添加模块过滤条件
        if (!module.empty()) {
            // action 字段格式为 "module:action"，使用 LIKE 模糊匹配
            where += " AND action LIKE ?";
            params.push_back(module + ":%");
        }
        // 如果指定了操作者 ID，添加操作者过滤条件
        if (!operatorId.empty()) {
            where += " AND username=?";
            params.push_back(operatorId);
        }

        // 查询符合条件的总记录数
        auto cntR = db.query("SELECT COUNT(*) AS c FROM oplog WHERE " + where, params);
        // 获取总数（如果查询失败返回 0）
        int total = cntR.empty() ? 0 : std::stoi(cntR[0]["c"]);

        // 复制参数列表用于分页查询
        auto pp = params;
        // 添加 LIMIT 参数
        pp.push_back(std::to_string(size));
        // 添加 OFFSET 参数
        pp.push_back(std::to_string(offset));
        // 查询分页数据（按 ID 倒序排列）
        auto rows = db.query(
            // SQL 语句：查询日志记录，按 ID 倒序，分页显示
            "SELECT * FROM oplog WHERE " + where +
            " ORDER BY id DESC LIMIT ? OFFSET ?", pp);

        // 创建 JSON 数组用于存储日志记录
        Json::Value arr(Json::arrayValue);
        // 遍历查询结果
        for (auto &r : rows) {
            // 创建单条日志的 JSON 对象
            Json::Value item;
            // 日志 ID
            item["id"]            = std::stoi(r.at("id"));
            // 操作者类型（1=管理员, 2=商户）
            item["operator_type"] = std::stoi(r.at("user_type"));
            // 操作者 ID（用户名或商户 ID）
            item["operator_id"]   = r.at("username");
            // 操作动作（格式为 "module:action"，前端可拆分）
            item["action"]        = r.at("action");
            // 目标对象 ID
            item["target_id"]     = r.at("target");
            // 操作详情
            item["detail"]        = r.at("detail");
            // 操作者 IP 地址
            item["ip"]            = r.at("ip");
            // 创建时间戳
            item["created_at"]    = (Json::Int64)std::stoll(r.at("created_at"));
            // 添加到数组
            arr.append(item);
        }

        // 构建返回数据
        Json::Value data;
        // 日志记录列表
        data["list"] = arr;
        // 总记录数
        data["total"] = total;
        // 返回数据
        return data;
    }
// 类定义结束
};
