// WePay-Cpp — 管理后台: Webhook 配置管理控制器
// 职责：Webhook 端点的增删改查、测试推送等管理功能
//
// API 端点：
// GET  /admin/api/webhook/list       列表
// POST /admin/api/webhook/add        新增
// POST /admin/api/webhook/update     更新
// POST /admin/api/webhook/delete     删除
// POST /admin/api/webhook/test       测试推送
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // C 时间库
#include <vector> // 向量库
#include <map> // 映射库
#include <openssl/hmac.h> // OpenSSL HMAC 库
#include <openssl/evp.h> // OpenSSL EVP 库
#include <curl/curl.h> // libcurl 库
#include <json/json.h> // JSON 库
#include "../common/AjaxResult.h"
#include "../common/PayDb.h"
#include "../filters/AdminAuthFilter.h"

class AdminWebhookCtrl : public drogon::HttpController<AdminWebhookCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(AdminWebhookCtrl::list,    "/admin/api/webhook/list",   drogon::Get,  "AdminAuthFilter");
        ADD_METHOD_TO(AdminWebhookCtrl::add,     "/admin/api/webhook/add",    drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(AdminWebhookCtrl::update,  "/admin/api/webhook/update", drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(AdminWebhookCtrl::del,      "/admin/api/webhook/delete", drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(AdminWebhookCtrl::testPush, "/admin/api/webhook/test",  drogon::Post, "AdminAuthFilter");
    METHOD_LIST_END

    void list(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto &db = PayDb::instance();
        int page = safeInt(req->getParameter("page"), 1);
        int size = safeInt(req->getParameter("size"), 20);
        int offset = (page - 1) * size;
        std::string mchIdP = req->getParameter("mch_id");

        std::string where = "1=1";
        std::vector<std::string> params;
        if (!mchIdP.empty()) { where += " AND w.mch_id=?"; params.push_back(mchIdP); }

        auto cntR = db.query("SELECT COUNT(*) AS c FROM mch_webhook w WHERE " + where, params);
        int total = cntR.empty() ? 0 : std::stoi(cntR[0]["c"]);

        auto pp = params;
        pp.push_back(std::to_string(size));
        pp.push_back(std::to_string(offset));
        auto rows = db.query(
            "SELECT w.*,m.mch_no,m.mch_name FROM mch_webhook w "
            "LEFT JOIN merchant m ON m.id=w.mch_id WHERE " + where +
            " ORDER BY w.id DESC LIMIT ? OFFSET ?", pp);

        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value item;
            item["id"]          = std::stoi(r.at("id"));
            item["mch_id"]      = std::stoi(r.at("mch_id"));
            item["mch_no"]     = r.count("mch_no") ? r.at("mch_no") : "";
            item["mch_name"]   = r.count("mch_name") ? r.at("mch_name") : "";
            item["event_type"] = r.at("event_type");
            item["notify_url"]  = r.at("notify_url");
            item["sign_type"]   = r.count("sign_type") ? r.at("sign_type") : "HMAC-SHA256";
            item["state"]      = std::stoi(r.at("state"));
            item["retry_max"]   = std::stoi(r.at("retry_max"));
            item["timeout_sec"] = std::stoi(r.at("timeout_sec"));
            item["created_at"]  = (Json::Int64)std::stoll(r.at("created_at"));
            arr.append(item);
        }
        Json::Value data;
        data["list"] = arr; data["total"] = total;
        RESP_OK(cb, data);
    }

    void add(const drogon::HttpRequestPtr &req,
             std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "参数格式错误"); return; }
        auto &db = PayDb::instance();
        long long now = std::time(nullptr);

        int mchId     = (*body).get("mch_id", 0).asInt();
        std::string eventType = (*body).get("event_type", "").asString();
        std::string notifyUrl = (*body).get("notify_url", "").asString();
        std::string secretKey  = (*body).get("secret_key", "").asString();
        std::string signType   = (*body).get("sign_type", "HMAC-SHA256").asString();

        if (mchId <= 0 || eventType.empty() || notifyUrl.empty()) {
            RESP_ERR(cb, "参数不完整"); return;
        }

        // 自动生成密钥
        if (secretKey.empty()) {
            secretKey = generateSecret(32);
        }

        bool ok = db.exec(
            "INSERT INTO mch_webhook(mch_id,event_type,notify_url,secret_key,sign_type,"
            "state,retry_max,timeout_sec,created_at,updated_at) "
            "VALUES(?,?,?,?,?,1,3,30,?,?)",
            {std::to_string(mchId), eventType, notifyUrl, secretKey,
             signType, std::to_string(now), std::to_string(now)});
        if (!ok) { RESP_ERR(cb, "创建失败"); return; }

        Json::Value data;
        data["secret_key"] = secretKey; // 仅创建时返回一次
        RESP_OK(cb, data);
    }

    void update(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "参数格式错误"); return; }
        auto &db = PayDb::instance();
        long long now = std::time(nullptr);

        int64_t id     = (*body).get("id", 0).asInt64();
        std::string notifyUrl = (*body).get("notify_url", "").asString();
        std::string secretKey = (*body).get("secret_key", "").asString();
        std::string signType  = (*body).get("sign_type", "HMAC-SHA256").asString();
        int state      = (*body).get("state", 1).asInt();
        int retryMax   = (*body).get("retry_max", 3).asInt();
        int timeoutSec = (*body).get("timeout_sec", 30).asInt();

        if (id <= 0) { RESP_ERR(cb, "ID错误"); return; }

        std::vector<std::string> params;
        std::string sql = "UPDATE mch_webhook SET updated_at=?";
        params.push_back(std::to_string(now));
        if (!notifyUrl.empty()) { sql += ",notify_url=?"; params.push_back(notifyUrl); }
        if (!secretKey.empty()) { sql += ",secret_key=?"; params.push_back(secretKey); }
        if (!signType.empty()) { sql += ",sign_type=?"; params.push_back(signType); }
        sql += ",state=?,retry_max=?,timeout_sec=?"; params.push_back(std::to_string(state));
        params.push_back(std::to_string(retryMax));
        params.push_back(std::to_string(timeoutSec));
        sql += " WHERE id=?"; params.push_back(std::to_string(id));

        if (db.exec(sql, params)) RESP_MSG(cb, "更新成功");
        else RESP_ERR(cb, "更新失败");
    }

    void del(const drogon::HttpRequestPtr &req,
             std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "参数格式错误"); return; }
        int64_t id = (*body).get("id", 0).asInt64();
        if (id <= 0) { RESP_ERR(cb, "ID错误"); return; }
        if (PayDb::instance().exec("DELETE FROM mch_webhook WHERE id=?", {std::to_string(id)}))
            RESP_MSG(cb, "删除成功");
        else RESP_ERR(cb, "删除失败");
    }

    void testPush(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "参数格式错误"); return; }
        int64_t id = (*body).get("id", 0).asInt64();
        if (id <= 0) { RESP_ERR(cb, "ID错误"); return; }

        auto &db = PayDb::instance();
        auto row = db.queryOne(
            "SELECT * FROM mch_webhook WHERE id=? AND state=1", {std::to_string(id)});
        if (row.empty()) { RESP_ERR(cb, "Webhook不存在或已禁用"); return; }

        // 构造测试事件
        Json::Value payload;
        payload["event"] = "test";
        payload["event_type"] = row.at("event_type");
        payload["timestamp"] = (Json::Int64)std::time(nullptr);
        payload["data"]["order_id"] = "TEST" + std::to_string(std::time(nullptr));
        payload["data"]["amount"] = "0.01";
        payload["data"]["status"] = "SUCCESS";
        payload["data"]["message"] = "这是一条测试通知";

        // 签名
        std::string secretKey = row.at("secret_key");
        std::string signType  = row.count("sign_type") ? row.at("sign_type") : "HMAC-SHA256";
        std::string sign;
        if (signType == "HMAC-SHA256") {
            sign = hmacSha256(secretKey, payload.toStyledString());
        } else {
            sign = "RSA_NOT_IMPLEMENTED";
        }
        payload["sign"] = sign;

        // HTTP POST
        std::string respBody;
        bool ok = httpPost(row.at("notify_url"), payload.toStyledString(), respBody);

        Json::Value data;
        data["url"]    = row.at("notify_url");
        data["payload"] = payload;
        data["response"] = respBody;
        data["success"] = ok;
        RESP_OK(cb, data);
    }

private:
    static int safeInt(const std::string &s, int def) {
        try { return std::stoi(s); } catch (...) { return def; }
    }

    static std::string generateSecret(int len) {
        static const char chars[] =
            "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
        std::string s;
        s.reserve(len);
        for (int i = 0; i < len; ++i)
            s += chars[rand() % (sizeof(chars) - 1)];
        return s;
    }

    static std::string hmacSha256(const std::string &key, const std::string &data) {
        unsigned char h[32];
        HMAC(EVP_sha256(), key.data(), (int)key.size(),
             (unsigned char*)data.data(), data.size(), h, nullptr);
        static const char hex[] = "0123456789abcdef";
        std::string out;
        for (int i = 0; i < 32; ++i) {
            out += hex[h[i] >> 4];
            out += hex[h[i] & 0xf];
        }
        return out;
    }

    static bool httpPost(const std::string &url, const std::string &body,
                         std::string &response) {
        CURL *curl = curl_easy_init();
        if (!curl) return false;
        struct curl_slist *hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
            +[](char *ptr, size_t size, size_t nmemb, void *userdata) -> size_t {
                ((std::string*)userdata)->append(ptr, size * nmemb);
                return size * nmemb;
            });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        CURLcode res = curl_easy_perform(curl);
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);
        return res == CURLE_OK && code >= 200 && code < 300;
    }
};
