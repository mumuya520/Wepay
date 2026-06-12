// AiChatCtrl.h — AI 智能助手（移植自 ruoyi-cpp，适配 WePay 鉴权 + PayDb）
// 路由:  GET  /ai                  内嵌聊天 UI
//        GET  /admin/api/ai/info   模型状态
//        POST /admin/api/ai/session          新建会话
//        GET  /admin/api/ai/sessions         会话列表
//        DELETE /admin/api/ai/session/{id}   删除会话
//        GET  /admin/api/ai/session/{id}/messages  消息历史
//        POST /admin/api/ai/session/{id}/clear     清空
//        POST /admin/api/ai/chat             发送消息
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <json/json.h> // JSON 库
#include <optional> // 可选值
#include <string> // 字符串库
#include "../common/SimpleJwt.h" // JWT 令牌处理
#include "../common/PayDb.h" // 数据库操作
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "KoboldCppService.h" // KoboldCpp 服务

#ifndef AI_OK
// 成功响应宏
#define AI_OK(cb, data) do { \
    Json::Value _r; _r["code"]=200; _r["msg"]="ok"; _r["data"]=(data); \
    auto _resp=drogon::HttpResponse::newHttpJsonResponse(_r); cb(_resp); } while(0)
// 错误响应宏
#define AI_ERR(cb, msg) do { \
    Json::Value _r; _r["code"]=500; _r["msg"]=(msg); \
    auto _resp=drogon::HttpResponse::newHttpJsonResponse(_r); cb(_resp); } while(0)
// 未授权响应宏
#define AI_UNAUTH(cb) do { \
    Json::Value _r; _r["code"]=401; _r["msg"]="请先登录"; \
    auto _resp=drogon::HttpResponse::newHttpJsonResponse(_r); \
    _resp->setStatusCode(drogon::k401Unauthorized); cb(_resp); } while(0)
#endif

// AI 聊天控制器类
class AiChatCtrl : public drogon::HttpController<AiChatCtrl> {
public:
    METHOD_LIST_BEGIN // 路由列表开始
        ADD_METHOD_TO(AiChatCtrl::index,         "/ai",                              drogon::Get); // 聊天 UI
        ADD_METHOD_TO(AiChatCtrl::apiInfo,        "/admin/api/ai/info",               drogon::Get); // 模型状态
        ADD_METHOD_TO(AiChatCtrl::createSession,  "/admin/api/ai/session",            drogon::Post); // 创建会话
        ADD_METHOD_TO(AiChatCtrl::listSessions,   "/admin/api/ai/sessions",           drogon::Get); // 会话列表
        ADD_METHOD_TO(AiChatCtrl::deleteSession,  "/admin/api/ai/session/{id}",       drogon::Delete); // 删除会话
        ADD_METHOD_TO(AiChatCtrl::getMessages,    "/admin/api/ai/session/{id}/messages", drogon::Get); // 消息历史
        ADD_METHOD_TO(AiChatCtrl::clearSession,   "/admin/api/ai/session/{id}/clear", drogon::Post); // 清空会话
        ADD_METHOD_TO(AiChatCtrl::chat,           "/admin/api/ai/chat",               drogon::Post); // 发送消息
    METHOD_LIST_END // 路由列表结束

    // ── GET /ai  内嵌 HTML ────────────────────────────────────────────────
    void index(const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setContentTypeCode(drogon::CT_TEXT_HTML);
        resp->setBody(chatHtml());
        cb(resp);
    }

    // ── GET /admin/api/ai/info ────────────────────────────────────────────
    void apiInfo(const drogon::HttpRequestPtr& req,
                 std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        bool koboldOk = KoboldCppService::instance().isReady();
        Json::Value d;
        d["ready"]   = koboldOk;
        d["backend"] = "KoboldCpp";
        Json::Value models(Json::arrayValue);
        Json::Value m1; m1["id"]="kobold"; m1["name"]="本地模型(KoboldCpp)"; m1["available"]=koboldOk; models.append(m1);
        Json::Value m2; m2["id"]="xfai";   m2["name"]="讯飞星火(云端)";       m2["available"]=true; models.append(m2);
        d["models"] = models;
        AI_OK(cb, d);
    }

    // ── POST /admin/api/ai/session ────────────────────────────────────────
    void createSession(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto u = getUser(req);
        if (!u) { AI_UNAUTH(cb); return; }
        auto body = req->getJsonObject();
        std::string title = body ? (*body).get("title","新对话").asString() : "新对话";
        std::string model = body ? (*body).get("model","xfai").asString()   : "xfai";
        std::string modelName = (model == "kobold") ? "KoboldCpp" : "讯飞星火";
        if (title.size() > 100) title = title.substr(0, 100);

        auto& db = PayDb::instance();
        long long sid = 0;
        if (db.hasPg()) {
            auto rows = db.query(
                "INSERT INTO ai_chat_session(user_name,title,model_name,create_time,update_time) "
                "VALUES(?,?,?,NOW(),NOW()) RETURNING session_id",
                {u->name, title, modelName});
            if (rows.empty()) { AI_ERR(cb, "创建会话失败"); return; }
            sid = std::stoll(rows[0]["session_id"]);
        } else {
            db.exec("INSERT INTO ai_chat_session(user_name,title,model_name,"
                    "create_time,update_time) VALUES(?,?,?,datetime('now'),datetime('now'))",
                    {u->name, title, modelName});
            sid = db.lastInsertId();
        }
        Json::Value d; d["session_id"] = (Json::Int64)sid; d["title"] = title;
        AI_OK(cb, d);
    }

    // ── GET /admin/api/ai/sessions ────────────────────────────────────────
    void listSessions(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto u = getUser(req);
        if (!u) { AI_UNAUTH(cb); return; }
        auto rows = PayDb::instance().query(
            "SELECT session_id,title,model_name,update_time FROM ai_chat_session "
            "WHERE user_name=? ORDER BY update_time DESC LIMIT 50",
            {u->name});
        Json::Value list(Json::arrayValue);
        for (auto& r : rows) {
            Json::Value s;
            s["session_id"]  = r["session_id"];
            s["title"]       = r["title"];
            s["model_name"]  = r["model_name"];
            s["update_time"] = r["update_time"];
            list.append(s);
        }
        AI_OK(cb, list);
    }

    // ── DELETE /admin/api/ai/session/{id} ─────────────────────────────────
    void deleteSession(const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                       long long id) {
        auto u = getUser(req);
        if (!u) { AI_UNAUTH(cb); return; }
        auto& db = PayDb::instance();
        db.exec("DELETE FROM ai_chat_message WHERE session_id=?", {std::to_string(id)});
        db.exec("DELETE FROM ai_chat_session WHERE session_id=? AND user_name=?",
                {std::to_string(id), u->name});
        Json::Value d; d["deleted"] = true;
        AI_OK(cb, d);
    }

    // ── GET /admin/api/ai/session/{id}/messages ───────────────────────────
    void getMessages(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                     long long id) {
        auto u = getUser(req);
        if (!u) { AI_UNAUTH(cb); return; }
        auto rows = PayDb::instance().query(
            "SELECT msg_id,role,content,create_time FROM ai_chat_message "
            "WHERE session_id=? ORDER BY msg_id ASC",
            {std::to_string(id)});
        Json::Value list(Json::arrayValue);
        for (auto& r : rows) {
            Json::Value m;
            m["msg_id"]      = r["msg_id"];
            m["role"]        = r["role"];
            m["content"]     = r["content"];
            m["create_time"] = r["create_time"];
            list.append(m);
        }
        AI_OK(cb, list);
    }

    // ── POST /admin/api/ai/session/{id}/clear ─────────────────────────────
    void clearSession(const drogon::HttpRequestPtr& req,
                      std::function<void(const drogon::HttpResponsePtr&)>&& cb,
                      long long id) {
        auto u = getUser(req);
        if (!u) { AI_UNAUTH(cb); return; }
        auto& db = PayDb::instance();
        db.exec("DELETE FROM ai_chat_message WHERE session_id=?", {std::to_string(id)});
        db.exec("UPDATE ai_chat_session SET title='新对话',update_time=NOW() "
                "WHERE session_id=? AND user_name=?",
                {std::to_string(id), u->name});
        Json::Value d; d["cleared"] = true;
        AI_OK(cb, d);
    }

    // ── POST /admin/api/ai/chat ───────────────────────────────────────────
    void chat(const drogon::HttpRequestPtr& req,
              std::function<void(const drogon::HttpResponsePtr&)>&& cb) {
        auto u = getUser(req);
        if (!u) { AI_UNAUTH(cb); return; }

        auto body = req->getJsonObject();
        if (!body) { AI_ERR(cb, "请求体格式错误"); return; }

        long long   sessionId = (*body).get("session_id",  0).asInt64();
        std::string userMsg   = (*body).get("message",    "").asString();
        std::string sysPrompt = (*body).get("system_prompt",
            "你是 WePay 支付系统的 AI 助手，请用中文简洁地回答问题。").asString();
        int         maxTok    = (*body).get("max_tokens",   512).asInt();
        float       temp      = (float)(*body).get("temperature", 0.7).asDouble();
        std::string model     = (*body).get("model",      "xfai").asString();

        if (userMsg.empty()) { AI_ERR(cb, "消息不能为空"); return; }
        if (userMsg.size() > 8000) { AI_ERR(cb, "消息过长，请精简后重试"); return; }

        auto& db = PayDb::instance();
        std::string modelName = (model == "kobold") ? "KoboldCpp" : "讯飞星火";

        // 1. 验证/自动创建会话
        if (sessionId == 0) {
            std::string autoTitle = userMsg.size() > 30 ? userMsg.substr(0,30)+"..." : userMsg;
            if (db.hasPg()) {
                auto ins = db.query(
                    "INSERT INTO ai_chat_session(user_name,title,model_name,create_time,update_time) "
                    "VALUES(?,?,?,NOW(),NOW()) RETURNING session_id",
                    {u->name, autoTitle, modelName});
                if (ins.empty()) { AI_ERR(cb, "创建会话失败"); return; }
                sessionId = std::stoll(ins[0]["session_id"]);
            } else {
                db.exec("INSERT INTO ai_chat_session(user_name,title,model_name,"
                        "create_time,update_time) VALUES(?,?,?,datetime('now'),datetime('now'))",
                        {u->name, autoTitle, modelName});
                sessionId = db.lastInsertId();
            }
        } else {
            auto chk = db.queryOne(
                "SELECT session_id FROM ai_chat_session WHERE session_id=? AND user_name=?",
                {std::to_string(sessionId), u->name});
            if (chk.empty()) { AI_ERR(cb, "会话不存在或无权限"); return; }
        }

        // 2. 保存用户消息
        db.exec("INSERT INTO ai_chat_message(session_id,role,content,create_time) "
                "VALUES(?,'user',?,NOW())",
                {std::to_string(sessionId), userMsg});

        // 3. 拉取最近20条历史（用于 prompt 拼接）
        auto hist = db.query(
            "SELECT role,content FROM ai_chat_message WHERE session_id=? ORDER BY msg_id DESC LIMIT 20",
            {std::to_string(sessionId)});
        bool firstTurn = (hist.size() <= 1);
        std::string titleStr = userMsg.size() > 30 ? userMsg.substr(0,30)+"..." : userMsg;

        // 4. 共用回调：保存回复 + 更新会话 + 响应
        long long sid = sessionId;
        auto finalize = [=, &db](const std::string& reply) mutable {
            auto& db2 = PayDb::instance();
            db2.exec("INSERT INTO ai_chat_message(session_id,role,content,create_time) "
                     "VALUES(?,'assistant',?,NOW())",
                     {std::to_string(sid), reply});
            db2.exec("UPDATE ai_chat_session SET update_time=NOW() WHERE session_id=?",
                     {std::to_string(sid)});
            if (firstTurn)
                db2.exec("UPDATE ai_chat_session SET title=? WHERE session_id=?",
                         {titleStr, std::to_string(sid)});
            Json::Value d; d["session_id"] = (Json::Int64)sid; d["reply"] = reply;
            AI_OK(cb, d);
        };

        if (model == "xfai") {
            // 讯飞星火（异步 Drogon 客户端）
            try {
                auto client = drogon::HttpClient::newHttpClient("https://api.pearktrue.cn");
                auto xfReq  = drogon::HttpRequest::newHttpRequest();
                xfReq->setPath("/api/xfai/");
                xfReq->setParameter("message", userMsg);
                xfReq->setMethod(drogon::Get);
                client->sendRequest(xfReq,
                    [finalize](drogon::ReqResult result,
                               const drogon::HttpResponsePtr& resp) mutable {
                        std::string reply;
                        if (result == drogon::ReqResult::Ok && resp) {
                            try {
                                auto j = resp->getJsonObject();
                                if (j && (*j)["code"].asInt() == 200)
                                    reply = (*j)["data"].get("answer","").asString();
                            } catch (...) {}
                        }
                        if (reply.empty()) reply = "⚠️ 讯飞星火暂时无法响应，请稍后重试。";
                        finalize(reply);
                    }, 15.0);
            } catch (...) {
                finalize("⚠️ 讯飞星火请求失败，请稍后重试。");
            }
        } else {
            // KoboldCpp（同步生成）
            std::string reply;
            if (!KoboldCppService::instance().isReady()) {
                reply = "⚠️ AI 引擎尚未就绪，请检查 KoboldCpp 服务状态。";
            } else {
                std::string prompt = sysPrompt + "\n\n";
                // 逆序拼接历史（hist 是 DESC 排列）
                for (int i = (int)hist.size()-1; i >= 0; --i)
                    prompt += (hist[i]["role"] == "user" ? "用户：" : "助手：")
                           + hist[i]["content"] + "\n";
                prompt += "助手：";
                reply = KoboldCppService::instance().generate(prompt, temp, maxTok);
                if (reply.empty()) reply = KoboldCppService::instance().lastError();
                if (reply.empty()) reply = "AI 未返回内容，请重试。";
                // 去掉开头多余的"助手："前缀
                if (reply.rfind("助手：", 0) == 0) reply = reply.substr(6);
                else if (reply.rfind("助手:", 0) == 0) reply = reply.substr(5);
            }
            finalize(reply);
        }
    }

private:
    struct AiUser { std::string name; };

    // 从 Authorization 头解析管理员身份
    static std::optional<AiUser> getUser(const drogon::HttpRequestPtr& req) {
        // 优先读 AdminAuthFilter 已注入的 X-Admin-User（通过 /admin/api/* 中间件时）
        std::string adminUser = req->getHeader("X-Admin-User");
        if (!adminUser.empty()) return AiUser{adminUser};

        std::string auth = req->getHeader("Authorization");
        if (auth.empty()) auth = req->getHeader("authorization");
        if (auth.empty()) return std::nullopt;
        try {
            std::string token   = SimpleJwt::fromHeader(auth);
            std::string subject = SimpleJwt::verify(token);
            return AiUser{subject};
        } catch (...) { return std::nullopt; }
    }

    // ── 内嵌聊天 UI（ChatGPT 风格，WePay 品牌配色）────────────────────────
    static std::string chatHtml() { return R"HTMLEOF(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>WePay AI 助手</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;background:#1a1a2e;color:#e0e0e0;height:100vh;display:flex;overflow:hidden}
body.in-iframe #sidebar{display:none!important}
#sidebar{width:260px;background:#16213e;display:flex;flex-direction:column;padding:12px;gap:8px;border-right:1px solid #0f3460;flex-shrink:0}
#newChatBtn{background:#0f3460;border:1px solid #1677ff;color:#1677ff;padding:10px;border-radius:8px;cursor:pointer;font-size:14px;transition:.2s}
#newChatBtn:hover{background:#1677ff;color:#fff}
#sessionList{flex:1;overflow-y:auto;display:flex;flex-direction:column;gap:4px}
.session-item{padding:8px 10px;border-radius:6px;cursor:pointer;font-size:13px;color:#b0b0c0;display:flex;justify-content:space-between;align-items:center;word-break:break-all}
.session-item:hover,.session-item.active{background:#0f3460;color:#fff}
.session-item .del-btn{opacity:0;font-size:16px;color:#f56c6c;flex-shrink:0;margin-left:4px}
.session-item:hover .del-btn{opacity:1}
#modelStatus{font-size:11px;color:#666;padding:8px 2px;border-top:1px solid #0f3460}
#modelStatus span{color:#1677ff}
#main{flex:1;display:flex;flex-direction:column;overflow:hidden}
#chatTitle{padding:14px 20px;border-bottom:1px solid #0f3460;font-size:15px;font-weight:600;color:#69b1ff;background:#16213e;display:flex;align-items:center;gap:12px}
#chatTitle .spacer{flex:1}
#activeModelBadge{font-size:12px;color:#dbeafe;background:#1e3a5f;border:1px solid #315a87;border-radius:999px;padding:4px 10px}
#inlineNewBtn{display:none;background:#0f3460;border:1px solid #1677ff;color:#1677ff;padding:6px 14px;border-radius:6px;cursor:pointer;font-size:13px;transition:.2s}
#inlineNewBtn:hover{background:#1677ff;color:#fff}
body.in-iframe #inlineNewBtn{display:inline-block}
#messages{flex:1;overflow-y:auto;padding:20px;display:flex;flex-direction:column;gap:16px}
.msg{display:flex;gap:12px;max-width:820px;align-self:flex-start}
.msg.user{flex-direction:row-reverse;align-self:flex-end}
.avatar{width:36px;height:36px;border-radius:50%;display:flex;align-items:center;justify-content:center;font-size:16px;flex-shrink:0}
.msg.user .avatar{background:#1677ff}
.msg.assistant .avatar{background:#52c41a}
.bubble{padding:10px 14px;border-radius:12px;font-size:14px;line-height:1.6;max-width:680px;white-space:pre-wrap;word-break:break-word}
.msg.user .bubble{background:#0f3460;border-bottom-right-radius:2px}
.msg.assistant .bubble{background:#1e293b;border-bottom-left-radius:2px}
.bubble.typing::after{content:'▋';animation:blink .8s infinite}
@keyframes blink{0%,100%{opacity:1}50%{opacity:0}}
#inputArea{padding:16px 20px;border-top:1px solid #0f3460;background:#16213e;display:flex;gap:10px;align-items:flex-end}
#msgInput{flex:1;background:#1e293b;border:1px solid #0f3460;color:#e0e0e0;border-radius:10px;padding:10px 14px;font-size:14px;resize:none;max-height:160px;min-height:44px;outline:none;font-family:inherit;line-height:1.5}
#msgInput:focus{border-color:#1677ff}
#sendBtn{background:#1677ff;color:#fff;border:none;border-radius:10px;padding:10px 18px;cursor:pointer;font-size:14px;transition:.2s;height:44px}
#sendBtn:hover:not(:disabled){background:#4096ff}
#sendBtn:disabled{background:#1e3a5f;cursor:not-allowed}
#settingsRow{padding:4px 20px 0;display:flex;gap:12px;align-items:center;font-size:12px;color:#666}
#settingsRow label{display:flex;align-items:center;gap:4px}
#settingsRow input[type=range]{width:80px;accent-color:#1677ff}
#settingsRow select{background:#1e293b;color:#69b1ff;border:1px solid #0f3460;border-radius:4px}
::-webkit-scrollbar{width:5px}
::-webkit-scrollbar-thumb{background:#0f3460;border-radius:3px}
</style>
</head>
<body>
<div id="sidebar">
  <button id="newChatBtn">＋ 新对话</button>
  <div id="sessionList"></div>
  <div id="modelStatus">AI引擎：<span id="modelReady">检测中...</span></div>
</div>
<div id="main">
  <div id="chatTitle">
    <span>WePay AI 助手</span><span class="spacer"></span>
    <span id="activeModelBadge">当前模型：讯飞星火</span>
    <button id="inlineNewBtn">＋ 新对话</button>
  </div>
  <div id="messages">
    <div id="welcome" style="flex:1;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:16px;color:#555;padding:40px">
      <h2 style="color:#1677ff;font-size:22px">👋 你好，我是 WePay AI 助手</h2>
      <p style="font-size:14px;text-align:center;max-width:400px;line-height:1.8">可以帮你查询订单、分析支付数据、回答业务问题。</p>
    </div>
  </div>
  <div id="settingsRow">
    <label>模型 <select id="modelSel">
      <option value="xfai">⚡ 讯飞星火(云端)</option>
      <option value="kobold">🖥️ 本地模型(KoboldCpp)</option>
    </select></label>
    <label>温度 <input type="range" id="tempRange" min="0" max="100" value="70"><span id="tempVal">0.7</span></label>
    <label>最大字数 <select id="maxTokSel">
      <option value="256">256</option><option value="512" selected>512</option><option value="1024">1024</option>
    </select></label>
  </div>
  <div id="inputArea">
    <textarea id="msgInput" placeholder="输入消息… (Ctrl+Enter 发送)" rows="1"></textarea>
    <button id="sendBtn">发送</button>
  </div>
</div>
<script>
function getCookie(n){const m=document.cookie.match(new RegExp('(?:^|; )'+n+'=([^;]*)'));return m?decodeURIComponent(m[1]):''}
const token=getCookie('Admin-Token')||new URLSearchParams(location.search).get('token')
  ||localStorage.getItem('Admin-Token')||sessionStorage.getItem('Admin-Token')||'';
let currentSession=null,sending=false;
const API=(path,opt={})=>{
  const url='/admin/api/ai'+path;
  const headers={...(opt.headers||{})};
  if(token) headers['Authorization']='Bearer '+token;
  if(!headers['Content-Type']&&!(opt.body instanceof FormData)) headers['Content-Type']='application/json';
  return fetch(url,{...opt,headers}).then(r=>r.json());
};
const $=id=>document.getElementById(id);
function scrollBottom(){const m=$('messages');m.scrollTop=m.scrollHeight;}
function getModelLabel(v){return v==='kobold'?'KoboldCpp':'讯飞星火';}
function updateActiveModel(){$('activeModelBadge').textContent='当前模型：'+getModelLabel($('modelSel').value);}
async function checkModel(){
  try{
    const r=await API('/info');
    const models=(r.data&&r.data.models)||[];
    const byId={};models.forEach(m=>byId[m.id]=m);
    const koboldOk=!!(byId.kobold&&byId.kobold.available);
    $('modelReady').innerHTML=
      `讯飞:<span style="color:${true?'#4caf50':'#f44336'}">${true?'可用✓':'异常✗'}</span> `+
      `KoboldCpp:<span style="color:${koboldOk?'#4caf50':'#f44336'}">${koboldOk?'就绪✓':'离线✗'}</span>`;
  }catch(e){$('modelReady').textContent='连接失败';}
}
async function loadSessions(){
  const r=await API('/sessions');
  const list=r.data||[];
  const el=$('sessionList');el.innerHTML='';
  list.forEach(s=>{
    const div=document.createElement('div');
    div.className='session-item'+(currentSession===s.session_id?' active':'');
    div.innerHTML=`<span style="overflow:hidden;text-overflow:ellipsis;white-space:nowrap">${esc(s.title||'新对话')}</span>`
      +`<span class="del-btn" title="删除">×</span>`;
    div.querySelector('.del-btn').onclick=async e=>{
      e.stopPropagation();if(!confirm('删除此对话？'))return;
      await API('/session/'+s.session_id,{method:'DELETE'});
      if(currentSession===s.session_id){currentSession=null;showWelcome();}
      loadSessions();
    };
    div.onclick=()=>openSession(s.session_id,s.title);
    el.appendChild(div);
  });
}
function esc(s){const d=document.createElement('div');d.textContent=s;return d.innerHTML;}
async function openSession(id,title){
  currentSession=id;$('chatTitle').firstChild.textContent=title||'对话';
  const r=await API('/session/'+id+'/messages');
  const el=$('messages');el.innerHTML='';
  (r.data||[]).forEach(m=>appendMsg(m.role,m.content));
  scrollBottom();loadSessions();
}
function showWelcome(){
  $('chatTitle').innerHTML=`<span>WePay AI 助手</span><span class="spacer"></span><span id="activeModelBadge">当前模型：${getModelLabel($('modelSel').value)}</span><button id="inlineNewBtn">＋ 新对话</button>`;
  $('inlineNewBtn').onclick=doNewChat;
  $('messages').innerHTML=`<div style="flex:1;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:16px;color:#555;padding:40px">
    <h2 style="color:#1677ff;font-size:22px">👋 你好，我是 WePay AI 助手</h2>
    <p style="font-size:14px;text-align:center;max-width:400px;line-height:1.8">可以帮你查询订单、分析支付数据、回答业务问题。</p></div>`;
}
const doNewChat=async()=>{
  const r=await API('/session',{method:'POST',body:JSON.stringify({title:'新对话',model:$('modelSel').value})});
  if(r.code===200){await openSession(r.data.session_id,'新对话');loadSessions();}
  else if(r.code===401){alert('请先登录系统后再使用 AI 助手');}
};
function appendMsg(role,content,typing=false){
  const el=$('messages');
  const div=document.createElement('div');div.className='msg '+role;
  const icon=role==='user'?'👤':'🤖';
  div.innerHTML=`<div class="avatar">${icon}</div><div class="bubble${typing?' typing':''}">${esc(content)}</div>`;
  el.appendChild(div);return div.querySelector('.bubble');
}
async function send(){
  if(sending)return;
  const input=$('msgInput');const msg=input.value.trim();
  if(!msg)return;
  if(!token){alert('请先登录系统后再使用 AI 助手');return;}
  sending=true;$('sendBtn').disabled=true;input.value='';input.style.height='auto';
  appendMsg('user',msg);const bubble=appendMsg('assistant','',true);scrollBottom();
  try{
    const r=await API('/chat',{method:'POST',body:JSON.stringify({
      session_id:currentSession||0,message:msg,
      temperature:$('tempRange').value/100,max_tokens:parseInt($('maxTokSel').value),
      model:$('modelSel').value
    })});
    if(r.code===200){
      if(!currentSession){currentSession=r.data.session_id;loadSessions();}
      bubble.classList.remove('typing');bubble.textContent=r.data.reply;
    }else if(r.code===401){bubble.classList.remove('typing');bubble.textContent='⚠️ 请先登录系统';}
    else{bubble.classList.remove('typing');bubble.textContent='错误：'+(r.msg||'未知错误');}
  }catch(e){bubble.classList.remove('typing');bubble.textContent='网络错误：'+e.message;}
  sending=false;$('sendBtn').disabled=false;scrollBottom();loadSessions();
}
$('newChatBtn').onclick=doNewChat;$('inlineNewBtn').onclick=doNewChat;$('sendBtn').onclick=send;
$('modelSel').onchange=updateActiveModel;$('tempRange').oninput=e=>$('tempVal').textContent=(e.target.value/100).toFixed(2);
$('msgInput').addEventListener('keydown',e=>{if(e.key==='Enter'&&(e.ctrlKey||e.metaKey)){e.preventDefault();send();}});
updateActiveModel();checkModel();loadSessions();showWelcome();
</script>
</body></html>
)HTMLEOF"; }
};
