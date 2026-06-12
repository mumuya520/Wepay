// AiSecurityGateway.h — AI 智能防护网关
// 全流量分析: SQLi / XSS / 路径穿越 / SSRF / 命令注入 / 扫描器检测
// 高风险 → 403 拦截；中风险 → 放行但记录；边界 → 可选 LLM 二次分析
// 管理接口: GET /admin/api/ai/threats  (threat log + 统计)
#pragma once // 防止头文件重复包含
#include <drogon/drogon.h> // Drogon 框架
#include <json/json.h> // JSON 库
#include <regex> // 正则表达式
#include <string> // 字符串库
#include <vector> // 向量容器
#include <algorithm> // 算法库
#include <cctype> // 字符类型库
#include <atomic> // 原子操作
#include "../common/PayDb.h" // 数据库操作
#include "../common/SimpleJwt.h" // JWT 令牌处理
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "KoboldCppService.h" // KoboldCpp 服务

// AI 安全网关控制器类
class AiSecurityGateway : public drogon::HttpController<AiSecurityGateway> {
public:
    METHOD_LIST_BEGIN // 路由列表开始
        ADD_METHOD_TO(AiSecurityGateway::threats,  "/admin/api/ai/threats",  drogon::Get); // 威胁日志
        ADD_METHOD_TO(AiSecurityGateway::stats,    "/admin/api/ai/threats/stats", drogon::Get); // 统计信息
        ADD_METHOD_TO(AiSecurityGateway::clearLog, "/admin/api/ai/threats/clear", drogon::Post); // 清空日志
    METHOD_LIST_END // 路由列表结束

    // ── 配置 ──────────────────────────────────────────────────────────────
    // 安全网关配置结构体
    struct Config {
        bool enabled        = true; // 是否启用
        int  blockThreshold = 80;   // score >= 80 → 403 拦截
        int  logThreshold   = 40;   // score >= 40 → 记录日志
        bool llmAnalysis    = false; // 是否对边界请求启用 LLM 二次分析
        int  llmThresholdLo = 50;   // LLM 分析下限
        int  llmThresholdHi = 79;   // LLM 分析上限
        // 不分析这些路径前缀（支付回调/设备上报等）
        std::vector<std::string> skipPrefixes { // 跳过分析的路径前缀
            "/gateway/", "/notify/", "/health", "/device/heartbeat" // 网关、回调、健康检查、设备心跳
        };
    };

    static Config& cfg() { static Config c; return c; }

    static void setup(const Json::Value& appCfg) {
        if (!appCfg.isMember("ai")) return;
        const auto& ai = appCfg["ai"];
        KoboldCppService::instance().setPort(ai.get("kobold_port", 5001).asInt());
        if (ai.isMember("security_gateway")) {
            const auto& sg = ai["security_gateway"];
            cfg().enabled        = sg.get("enabled",         true).asBool();
            cfg().blockThreshold = sg.get("block_threshold", 80).asInt();
            cfg().logThreshold   = sg.get("log_threshold",   40).asInt();
            cfg().llmAnalysis    = sg.get("llm_analysis",    false).asBool();
        }
        if (cfg().enabled) {
            registerAdvice();
            LOG_INFO << "[AiSecurityGateway] 已启用 block>=" << cfg().blockThreshold
                     << " log>=" << cfg().logThreshold
                     << (cfg().llmAnalysis ? " +LLM" : "");
        }
    }

    // ── 威胁日志接口 ───────────────────────────────────────────────────────
    void threats(const drogon::HttpRequestPtr& req,
                 std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        if (!checkAuth(req)) { RESP_401(cb); return; }
        int page = std::max(1, std::stoi(req->getParameter("page").empty() ? "1" : req->getParameter("page")));
        int size = std::min(100, std::stoi(req->getParameter("size").empty() ? "20" : req->getParameter("size")));
        int offset = (page - 1) * size;
        std::string type = req->getParameter("type");

        std::string sql = "SELECT id,ip,path,method,threat_type,score,action,detail,create_time "
                          "FROM ai_threat_log";
        std::vector<std::string> params;
        if (!type.empty()) { sql += " WHERE threat_type=?"; params.push_back(type); }
        sql += " ORDER BY id DESC LIMIT ? OFFSET ?";
        params.push_back(std::to_string(size));
        params.push_back(std::to_string(offset));

        auto rows = PayDb::instance().query(sql, params);
        Json::Value list(Json::arrayValue);
        for (auto& r : rows) {
            Json::Value o;
            o["id"]          = r["id"];
            o["ip"]          = r["ip"];
            o["path"]        = r["path"];
            o["method"]      = r["method"];
            o["threat_type"] = r["threat_type"];
            o["score"]       = r["score"];
            o["action"]      = r["action"];
            o["detail"]      = r["detail"];
            o["create_time"] = r["create_time"];
            list.append(o);
        }
        auto total = PayDb::instance().queryScalar(
            type.empty() ? "SELECT COUNT(*) FROM ai_threat_log"
                         : "SELECT COUNT(*) FROM ai_threat_log WHERE threat_type=?",
            type.empty() ? std::vector<std::string>{} : std::vector<std::string>{type},
            "0");
        Json::Value d;
        d["list"]  = list;
        d["total"] = total;
        d["page"]  = page;
        d["size"]  = size;
        RESP_OK(cb, d);
    }

    void stats(const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        if (!checkAuth(req)) { RESP_401(cb); return; }
        auto& db = PayDb::instance();
        Json::Value d;
        d["total"]   = db.queryScalar("SELECT COUNT(*) FROM ai_threat_log", {}, "0");
        d["blocked"] = db.queryScalar("SELECT COUNT(*) FROM ai_threat_log WHERE action='BLOCK'", {}, "0");
        d["logged"]  = db.queryScalar("SELECT COUNT(*) FROM ai_threat_log WHERE action='LOG'", {}, "0");
        d["today"]   = db.queryScalar(
            "SELECT COUNT(*) FROM ai_threat_log WHERE create_time >= CURRENT_DATE", {}, "0");
        // 按类型统计
        auto byType = db.query(
            "SELECT threat_type, COUNT(*) as cnt FROM ai_threat_log GROUP BY threat_type ORDER BY cnt DESC", {});
        Json::Value types(Json::arrayValue);
        for (auto& r : byType) {
            Json::Value o; o["type"] = r["threat_type"]; o["count"] = r["cnt"];
            types.append(o);
        }
        d["by_type"] = types;
        RESP_OK(cb, d);
    }

    void clearLog(const drogon::HttpRequestPtr& req,
                  std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        if (!checkAuth(req)) { RESP_401(cb); return; }
        // 只允许超管清除
        std::string isSuper = req->getHeader("X-Admin-Super");
        if (isSuper != "1") { RESP_ERR(cb, "无权限"); return; }
        PayDb::instance().exec("DELETE FROM ai_threat_log WHERE create_time < NOW() - INTERVAL '30 days'", {});
        Json::Value d; d["cleared"] = true;
        RESP_OK(cb, d);
    }

private:
    // ── 规则集 ────────────────────────────────────────────────────────────
    struct Rule {
        std::string type;
        std::vector<std::string> patterns;
        int score;
    };

    static const std::vector<Rule>& rules() {
        static std::vector<Rule> r = {
            {"SQLi", {
                "union.*select", "select.*from", "insert.*into", "drop.*table",
                "delete.*from", "update.*set", "exec(", "execute(", "xp_",
                "or.*1=1", "or.*'1'='1", "' or '", "-- ", "/*", "*/",
                "sleep(", "waitfor.*delay", "benchmark(", "load_file(",
                "into.*outfile", "information_schema", "sys.tables"
            }, 75},
            {"XSS", {
                "<script", "javascript:", "vbscript:", "onerror=", "onload=",
                "onclick=", "onmouseover=", "onfocus=", "eval(", "alert(",
                "document.cookie", "document.write", "window.location",
                "<iframe", "<object", "<embed", "<svg.*onload"
            }, 70},
            {"PathTraversal", {
                "../", "..\\", "/etc/passwd", "/etc/shadow", "windows/system32",
                "win.ini", "boot.ini", "/proc/self", "/var/www", "%2e%2e",
                "..%2f", "%252e", "..../"
            }, 80},
            {"SSRF", {
                "169.254.169.254", "metadata.google", "169.254.170.2",
                "file://", "dict://", "gopher://", "ftp://127",
                "http://localhost", "http://127.0.0.1", "http://0.0.0.0",
                "@127.0.0.1", "@localhost"
            }, 85},
            {"CmdInject", {
                ";cat ", ";ls ", ";id ", ";pwd ", "| cat", "| ls",
                "& dir", "& type", "`id`", "`whoami`", "$(id)",
                "$(whoami)", ";wget ", ";curl ", ";bash ", ";sh "
            }, 90},
            {"Scanner", {
                "sqlmap", "nikto", "nmap", "masscan", "zgrab",
                "python-requests/2.2", "go-http-client/1.1",
                "curl/7.29", "wget/1.1", "hydra", "medusa",
                "burpsuite", "w3af", "acunetix", "nessus"
            }, 60},
            {"Log4Shell", {
                "${jndi:", "${${::-j}${::-n}${::-d}${::-i}:", "${j${::-n}di",
                "${jnd${::-i}:", "${j${lower:n}di:", "jndi:ldap",
                "jndi:rmi", "jndi:dns"
            }, 95},
        };
        return r;
    }

    // ── 评分引擎 ──────────────────────────────────────────────────────────
    struct ScanResult {
        int score = 0;
        std::string type;
        std::string detail;
    };

    static ScanResult scan(const std::string& target) {
        std::string lower = target;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        // URL decode common encodings
        replaceAll(lower, "%3c", "<"); replaceAll(lower, "%3e", ">");
        replaceAll(lower, "%27", "'"); replaceAll(lower, "%22", "\"");
        replaceAll(lower, "%2f", "/"); replaceAll(lower, "%5c", "\\");
        replaceAll(lower, "%28", "("); replaceAll(lower, "%29", ")");
        replaceAll(lower, "+", " ");

        ScanResult best;
        for (auto& rule : rules()) {
            for (auto& pat : rule.patterns) {
                if (lower.find(pat) != std::string::npos) {
                    if (rule.score > best.score) {
                        best.score  = rule.score;
                        best.type   = rule.type;
                        best.detail = pat;
                    }
                }
            }
        }
        return best;
    }

    static void replaceAll(std::string& s, const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    }

    // ── 注册 pre-routing advice ────────────────────────────────────────────
    static void registerAdvice() {
        drogon::app().registerPreRoutingAdvice(
            [](const drogon::HttpRequestPtr& req,
               drogon::AdviceCallback&&      cb,
               drogon::AdviceChainCallback&& next) {

            const std::string& path = req->path();

            // 跳过豁免路径
            for (auto& pfx : cfg().skipPrefixes) {
                if (path.rfind(pfx, 0) == 0) { next(); return; }
            }

            // 拼接检测目标
            std::string target = path;
            if (!req->getQuery().empty()) target += "?" + req->getQuery();
            target += " " + req->getHeader("User-Agent");

            // body 检测（限 POST/PUT）
            std::string body;
            if (req->getMethod() == drogon::Post || req->getMethod() == drogon::Put) {
                body = std::string(req->getBody());
                if (body.size() > 4096) body = body.substr(0, 4096);
                target += " " + body;
            }

            auto result = scan(target);
            int  score  = result.score;

            // 边界 → 可选 LLM 二次分析
            if (cfg().llmAnalysis && score >= cfg().llmThresholdLo && score <= cfg().llmThresholdHi) {
                int llmScore = KoboldCppService::instance().analyzeRequest(
                    req->methodString(), path, req->getQuery(), body);
                if (llmScore > 0) {
                    score = std::max(score, llmScore);
                    result.detail += " [LLM:" + std::to_string(llmScore) + "]";
                }
            }

            if (score >= cfg().logThreshold) {
                std::string action = score >= cfg().blockThreshold ? "BLOCK" : "LOG";
                logThreat(req->getPeerAddr().toIp(), path,
                          req->methodString(), result.type, score, action, result.detail);

                if (action == "BLOCK") {
                    Json::Value j;
                    j["code"] = 403;
                    j["msg"]  = "请求被安全策略拦截";
                    auto resp = drogon::HttpResponse::newHttpJsonResponse(j);
                    resp->setStatusCode(drogon::k403Forbidden);
                    cb(resp);
                    return;
                }
            }
            next();
        });
    }

    static void logThreat(const std::string& ip, const std::string& path,
                          const std::string& method, const std::string& type,
                          int score, const std::string& action, const std::string& detail) {
        try {
            PayDb::instance().exec(
                "INSERT INTO ai_threat_log(ip,path,method,threat_type,score,action,detail,create_time) "
                "VALUES(?,?,?,?,?,?,?,NOW())",
                {ip, path, method, type, std::to_string(score), action, detail});
        } catch (...) {}
    }

    // ── 鉴权（复用 AdminAuthFilter 逻辑）────────────────────────────────
    static bool checkAuth(const drogon::HttpRequestPtr& req) {
        std::string auth = req->getHeader("Authorization");
        if (auth.empty()) auth = req->getHeader("authorization");
        if (auth.empty()) return false;
        try {
            std::string token = SimpleJwt::fromHeader(auth);
            SimpleJwt::verify(token);
            return true;
        } catch (...) { return false; }
    }
};

// 便于 main.cc 调用
inline void setupAiSecurityGateway(const Json::Value& cfg) {
    AiSecurityGateway::setup(cfg);
}
