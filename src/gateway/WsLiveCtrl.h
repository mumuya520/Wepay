// WePay-Cpp — 实时事件 WebSocket 控制器
// 监控端扫码后连接此 WebSocket，实时接收管理端所有动态
//
// 流程:
//   1. 管理端生成临时 live_token (有效期 5 分钟)
//   2. 前端展示 QR 码: ws(s)://host/ws/live?token=xxx
//   3. 监控端扫码 → 连接 WebSocket → 持续接收事件流
//
// 事件类型:
//   heartbeat    — 监控端心跳到达
//   push         — 收款推送到达
//   order_new    — 新订单创建
//   order_paid   — 订单支付成功
//   device_online — 设备上线
//   device_kick  — 设备被踢
//   config_save  — 管理端保存配置
//   callback_sent — 异步回调发送
//
// 端点:
//   WS  /ws/live?token=xxx          — 监控端实时连接
//   POST /admin/api/wepay/live/token — 管理端生成临时 token
//   GET  /admin/api/wepay/live/qr    — 管理端获取 QR 内容
#pragma once
#include <drogon/WebSocketController.h>
#include <drogon/HttpController.h>
#include "../common/WsBus.h"
#include "../common/PayDb.h"
#include "../common/AjaxResult.h"
#include "../common/Md5Utils.h"
#include <mutex>
#include <unordered_map>
#include <ctime>
#include <random>
#include <sstream>
#include <iomanip>

// ── Token 管理 (内存, 无需持久化) ─────────────────────────────
class LiveTokenStore {
public:
    struct Token {
        std::string value;
        std::string label;      // 备注 (e.g. "设备A扫码")
        long long   created;
        long long   expires;
        bool        used;       // 一次性: 连接后标记
    };

    static LiveTokenStore &instance() { static LiveTokenStore s; return s; }

    // 生成 token, ttl 秒
    std::string generate(int ttlSec = 300, const std::string &label = "") {
        std::string tok = randomHex(32);
        long long now = std::time(nullptr);
        std::lock_guard<std::mutex> lk(mtx_);
        // 清理过期的
        cleanup(now);
        tokens_[tok] = {tok, label, now, now + ttlSec, false};
        return tok;
    }

    // 消费 token (连接时调用), 返回是否有效
    bool consume(const std::string &tok) {
        std::lock_guard<std::mutex> lk(mtx_);
        long long now = std::time(nullptr);
        auto it = tokens_.find(tok);
        if (it == tokens_.end()) return false;
        if (it->second.expires < now) { tokens_.erase(it); return false; }
        if (it->second.used) return false;  // 已被使用
        it->second.used = true;
        return true;
    }

    // 校验 token 有效性 (不消费)
    bool valid(const std::string &tok) {
        std::lock_guard<std::mutex> lk(mtx_);
        long long now = std::time(nullptr);
        auto it = tokens_.find(tok);
        if (it == tokens_.end()) return false;
        return it->second.expires >= now && !it->second.used;
    }

    // 列出所有活跃 token
    std::vector<Token> list() {
        std::lock_guard<std::mutex> lk(mtx_);
        long long now = std::time(nullptr);
        cleanup(now);
        std::vector<Token> out;
        for (auto &kv : tokens_) out.push_back(kv.second);
        return out;
    }

    // 撤销 token
    void revoke(const std::string &tok) {
        std::lock_guard<std::mutex> lk(mtx_);
        tokens_.erase(tok);
    }

private:
    std::mutex mtx_;
    std::unordered_map<std::string, Token> tokens_;

    void cleanup(long long now) {
        for (auto it = tokens_.begin(); it != tokens_.end(); ) {
            if (it->second.expires < now) it = tokens_.erase(it);
            else ++it;
        }
    }

    static std::string randomHex(int bytes) {
        static std::random_device rd;
        static std::mt19937 gen(rd());
        std::uniform_int_distribution<> dist(0, 255);
        std::ostringstream oss;
        for (int i = 0; i < bytes; ++i)
            oss << std::hex << std::setw(2) << std::setfill('0') << dist(gen);
        return oss.str();
    }
};

// ── WebSocket 控制器: /ws/live ─────────────────────────────
class WsLiveCtrl : public drogon::WebSocketController<WsLiveCtrl> {
public:
    void handleNewConnection(const drogon::HttpRequestPtr &req,
                             const drogon::WebSocketConnectionPtr &conn) override {
        // 从 query 获取 token
        std::string token = req->getParameter("token");
        if (token.empty()) {
            conn->send(R"({"event":"error","data":{"msg":"缺少 token 参数"}})");
            conn->forceClose(); return;
        }
        // 验证并消费 token
        if (!LiveTokenStore::instance().consume(token)) {
            conn->send(R"({"event":"error","data":{"msg":"token 无效或已过期"}})");
            conn->forceClose(); return;
        }

        // 订阅 live:token 主题
        std::string topic = "live:" + token;
        WsBus::instance().subscribe(topic, conn);
        conn->setContext(std::make_shared<std::string>(topic));

        // 发送欢迎消息 + 当前系统快照
        Json::Value welcome;
        welcome["event"] = "connected";
        Json::Value snap;
        auto &db = PayDb::instance();
        snap["jkstate"]       = db.getSetting("wepay_jkstate", "0");
        snap["lastheart"]     = db.getSetting("wepay_lastheart", "0");
        snap["lastpay"]       = db.getSetting("wepay_lastpay", "0");
        snap["server_time"]   = (Json::Int64)std::time(nullptr);
        snap["live_clients"]  = (int)WsBus::instance().subscribers(topic);
        // 最近 5 条待支付订单
        auto orders = db.query(
            "SELECT order_id,pay_type,real_amount,created_at FROM pay_order "
            "WHERE state=0 ORDER BY created_at DESC LIMIT 5", {});
        Json::Value arr(Json::arrayValue);
        for (auto &o : orders) {
            Json::Value item;
            item["order_id"] = o.count("order_id") ? o["order_id"] : "";
            item["pay_type"] = o.count("pay_type") ? o["pay_type"] : "";
            item["amount"]   = o.count("real_amount") ? o["real_amount"] : "";
            arr.append(item);
        }
        snap["pending_orders"] = arr;
        welcome["data"] = snap;
        welcome["ts"]   = (Json::Int64)std::time(nullptr);

        Json::StreamWriterBuilder wb; wb["indentation"] = "";
        conn->send(Json::writeString(wb, welcome));

        LOG_INFO << "[ws/live] client connected token=" << token.substr(0, 8) << "..."
                 << " ip=" << req->getPeerAddr().toIp();
    }

    void handleNewMessage(const drogon::WebSocketConnectionPtr &conn,
                          std::string &&message,
                          const drogon::WebSocketMessageType &) override {
        if (message == "ping") conn->send("pong");
    }

    void handleConnectionClosed(const drogon::WebSocketConnectionPtr &conn) override {
        if (conn->hasContext()) {
            auto topic = conn->getContext<std::string>();
            if (topic) WsBus::instance().unsubscribe(*topic, conn);
        }
        WsBus::instance().unsubscribeAll(conn);
    }

    WS_PATH_LIST_BEGIN
        WS_PATH_ADD("/ws/live");
    WS_PATH_LIST_END
};

// ── HTTP 控制器: token 管理 API ─────────────────────────────
class WsLiveTokenCtrl : public drogon::HttpController<WsLiveTokenCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(WsLiveTokenCtrl::createToken,   "/admin/api/wepay/live/token",   drogon::Post);
        ADD_METHOD_TO(WsLiveTokenCtrl::revokeToken,    "/admin/api/wepay/live/revoke",  drogon::Post);
        ADD_METHOD_TO(WsLiveTokenCtrl::listTokens,     "/admin/api/wepay/live/tokens",  drogon::Get);
        ADD_METHOD_TO(WsLiveTokenCtrl::qrContent,      "/admin/api/wepay/live/qr",      drogon::Get);
        ADD_METHOD_TO(WsLiveTokenCtrl::livePage,       "/live",                         drogon::Get);
    METHOD_LIST_END

    // POST /admin/api/wepay/live/token
    //   Body: { ttl?: 300, label?: "设备A" }
    //   返回: { code:1, data: { token, expires, ws_url } }
    void createToken(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        int ttl = 300;
        std::string label;
        auto body = req->getJsonObject();
        if (body) {
            ttl   = (*body).get("ttl", 300).asInt();
            label = (*body).get("label", "").asString();
        }
        if (ttl < 60)   ttl = 60;
        if (ttl > 3600)  ttl = 3600;

        std::string token = LiveTokenStore::instance().generate(ttl, label);

        // 构建 ws_url
        auto &db = PayDb::instance();
        std::string siteUrl = db.getSetting("site_url", "");
        std::string wsUrl;
        if (!siteUrl.empty()) {
            // http:// → ws://, https:// → wss://
            if (siteUrl.substr(0, 8) == "https://")
                wsUrl = "wss://" + siteUrl.substr(8);
            else if (siteUrl.substr(0, 7) == "http://")
                wsUrl = "ws://" + siteUrl.substr(7);
            else
                wsUrl = "ws://" + siteUrl;
            if (wsUrl.back() == '/') wsUrl.pop_back();
            wsUrl += "/ws/live?token=" + token;
        }

        Json::Value data;
        data["code"] = 1;
        Json::Value d;
        d["token"]   = token;
        d["expires"] = (Json::Int64)(std::time(nullptr) + ttl);
        d["ttl"]     = ttl;
        d["ws_url"]  = wsUrl;
        d["label"]   = label;
        data["data"]  = d;
        cb(drogon::HttpResponse::newHttpJsonResponse(data));
    }

    // POST /admin/api/wepay/live/revoke  { token }
    void revokeToken(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "参数错误"); return; }
        std::string token = (*body).get("token", "").asString();
        if (token.empty()) { RESP_ERR(cb, "token 必填"); return; }
        LiveTokenStore::instance().revoke(token);
        RESP_MSG(cb, "已撤销");
    }

    // GET /admin/api/wepay/live/tokens — 列出活跃 token
    void listTokens(const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto tokens = LiveTokenStore::instance().list();
        Json::Value arr(Json::arrayValue);
        for (auto &t : tokens) {
            Json::Value v;
            v["token"]   = t.value.substr(0, 8) + "..."; // 只显示前8位
            v["token_full"] = t.value;
            v["label"]   = t.label;
            v["created"] = (Json::Int64)t.created;
            v["expires"] = (Json::Int64)t.expires;
            v["used"]    = t.used;
            v["clients"] = (int)WsBus::instance().subscribers("live:" + t.value);
            arr.append(v);
        }
        Json::Value r;
        r["code"] = 1;
        r["data"] = arr;
        cb(drogon::HttpResponse::newHttpJsonResponse(r));
    }

    // GET /admin/api/wepay/live/qr?ttl=300&label=设备A
    //   返回 QR 码内容 (ws_url 字符串), 自动生成 token
    void qrContent(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        int ttl = 300;
        std::string label;
        auto ttlStr = req->getParameter("ttl");
        if (!ttlStr.empty()) try { ttl = std::stoi(ttlStr); } catch (...) {}
        label = req->getParameter("label");
        if (ttl < 60) ttl = 60;
        if (ttl > 3600) ttl = 3600;

        std::string token = LiveTokenStore::instance().generate(ttl, label);
        auto &db = PayDb::instance();
        std::string siteUrl = db.getSetting("site_url", "");
        if (siteUrl.empty()) siteUrl = db.getSetting("siteUrl", "");
        // fallback: 优先用 x-forwarded-for(Vite proxy 会加) + 本地端口
        if (siteUrl.empty()) {
            std::string host = req->getHeader("host");
            bool isLocalhost = host.empty() ||
                host.find("localhost") != std::string::npos ||
                host.find("127.0.0.1") != std::string::npos;
            if (isLocalhost) {
                // Vite proxy 转发时会加 x-forwarded-for: 浏览器真实 IP
                std::string xff = req->getHeader("x-forwarded-for");
                if (!xff.empty()) {
                    // 取第一个 IP（可能多个逗号分隔）
                    auto comma = xff.find(',');
                    std::string clientIp = comma == std::string::npos ? xff : xff.substr(0, comma);
                    // 去掉空格
                    while (!clientIp.empty() && clientIp.front() == ' ') clientIp.erase(0, 1);
                    while (!clientIp.empty() && clientIp.back()  == ' ') clientIp.pop_back();
                    // 拼端口（用 localAddr 取监听端口，通常 8088）
                    std::string port = std::to_string(req->localAddr().toPort());
                    if (!clientIp.empty() && clientIp != "127.0.0.1" && clientIp != "::1") {
                        host = clientIp + ":" + port;
                    }
                }
            }
            if (host.empty()) host = "127.0.0.1:8088";
            std::string proto = req->getHeader("x-forwarded-proto");
            if (proto.empty()) proto = "http";
            siteUrl = proto + "://" + host;
        }
        std::string wsUrl;
        if (siteUrl.substr(0, 8) == "https://")
            wsUrl = "wss://" + siteUrl.substr(8);
        else if (siteUrl.substr(0, 7) == "http://")
            wsUrl = "ws://" + siteUrl.substr(7);
        else
            wsUrl = "ws://" + siteUrl;
        while (!wsUrl.empty() && wsUrl.back() == '/') wsUrl.pop_back();
        wsUrl += "/ws/live?token=" + token;

        // QR 给手机扫用: HTTP 页面地址，页面内 JS 自动连接 ws
        std::string qrUrl = siteUrl;
        while (!qrUrl.empty() && qrUrl.back() == '/') qrUrl.pop_back();
        qrUrl += "/live?token=" + token;

        Json::Value r;
        r["code"]   = 1;
        r["token"]  = token;
        r["ws_url"] = wsUrl;
        r["qr_url"] = qrUrl;
        r["expires"]= (Json::Int64)(std::time(nullptr) + ttl);
        cb(drogon::HttpResponse::newHttpJsonResponse(r));
    }

    // GET /live?token=xxx  — 手机扫码后打开的监控页面
    void livePage(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string token = req->getParameter("token");
        if (token.empty() || !LiveTokenStore::instance().valid(token)) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k403Forbidden);
            resp->setBody("Token 无效或已过期");
            cb(resp); return;
        }
        auto &db = PayDb::instance();
        std::string siteUrl = db.getSetting("site_url", "");
        if (siteUrl.empty()) siteUrl = db.getSetting("siteUrl", "");
        if (siteUrl.empty()) {
            std::string host = req->getHeader("host");
            bool isLocalhost = host.empty() ||
                host.find("localhost") != std::string::npos ||
                host.find("127.0.0.1") != std::string::npos;
            if (isLocalhost) {
                std::string xff = req->getHeader("x-forwarded-for");
                if (!xff.empty()) {
                    auto comma = xff.find(',');
                    std::string clientIp = comma == std::string::npos ? xff : xff.substr(0, comma);
                    while (!clientIp.empty() && clientIp.front() == ' ') clientIp.erase(0, 1);
                    while (!clientIp.empty() && clientIp.back()  == ' ') clientIp.pop_back();
                    std::string port = std::to_string(req->localAddr().toPort());
                    if (!clientIp.empty() && clientIp != "127.0.0.1" && clientIp != "::1") {
                        host = clientIp + ":" + port;
                    }
                }
            }
            if (host.empty()) host = "127.0.0.1:8088";
            std::string proto = req->getHeader("x-forwarded-proto");
            if (proto.empty()) proto = "http";
            siteUrl = proto + "://" + host;
        }
        while (!siteUrl.empty() && siteUrl.back() == '/') siteUrl.pop_back();
        std::string wsUrl;
        if (siteUrl.substr(0, 8) == "https://")
            wsUrl = "wss://" + siteUrl.substr(8);
        else if (siteUrl.substr(0, 7) == "http://")
            wsUrl = "ws://" + siteUrl.substr(7);
        else
            wsUrl = "ws://" + siteUrl;
        wsUrl += "/ws/live?token=" + token;

        std::string html = R"(<!DOCTYPE html><html lang="zh"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>WePay 实时监控</title>
<style>body{font-family:sans-serif;background:#0d1117;color:#c9d1d9;margin:0;padding:16px}
h2{color:#58a6ff;margin:0 0 8px}
#status{padding:6px 12px;border-radius:4px;display:inline-block;font-size:13px;margin-bottom:12px}
.ok{background:#1f6feb;color:#fff}.err{background:#da3633;color:#fff}.wait{background:#30363d;color:#8b949e}
#log{background:#161b22;border:1px solid #30363d;border-radius:6px;padding:12px;
     font-size:13px;line-height:1.6;height:60vh;overflow-y:auto;white-space:pre-wrap;word-break:break-all}
</style></head><body>
<h2>WePay 实时监控</h2>
<div id="status" class="wait">连接中…</div>
<div id="log">等待事件…</div>
<script>
var ws,reconnTimer,wsUrl=')";
        html += "\"" + wsUrl + "\"";
        html += R"(;
function connect(){
  ws=new WebSocket(wsUrl);
  ws.onopen=function(){
    document.getElementById('status').className='ok';
    document.getElementById('status').textContent='已连接';
  };
  ws.onmessage=function(e){
    var el=document.getElementById('log');
    var t=new Date().toLocaleTimeString();
    try{var j=JSON.parse(e.data);el.textContent+=t+' '+JSON.stringify(j)+'\n';}
    catch(_){el.textContent+=t+' '+e.data+'\n';}
    el.scrollTop=el.scrollHeight;
  };
  ws.onclose=function(){
    document.getElementById('status').className='err';
    document.getElementById('status').textContent='已断开，5秒后重连…';
    reconnTimer=setTimeout(connect,5000);
  };
  ws.onerror=function(){
    document.getElementById('status').className='err';
    document.getElementById('status').textContent='连接失败';
  };
}
connect();
</script></body></html>)";
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setContentTypeCode(drogon::CT_TEXT_HTML);
        resp->setBody(html);
        cb(resp);
    }
};
