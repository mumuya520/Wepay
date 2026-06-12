// WePay-Cpp — 管理后台: 二维码管理控制器
// 职责：二维码模板的增删改查、生成、扫描识别等功能
//
// 支持的二维码类型：
// 1. 静态二维码：固定支付地址
// 2. 动态二维码：按订单号生成，支持金额变化
// 3. 条形码：支持条形码识别和生成
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // C 时间库
#include <qrencode.h> // QR 码生成库
#include <ZXing/ReadBarcode.h> // 条形码识别库
#include <ZXing/Barcode.h> // 条形码库
#include <ZXing/ImageView.h> // 图像视图库
#include <ZXing/BarcodeFormat.h> // 条形码格式库
#include "../common/stb_image.h" // 图像处理库
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/AesGcmUtils.h" // AES-GCM 加密工具
#include "../filters/AdminAuthFilter.h"

class QrcodeMgrCtrl : public drogon::HttpController<QrcodeMgrCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(QrcodeMgrCtrl::list,   "/admin/api/qrcode/list",        drogon::Get,    "AdminAuthFilter");
        ADD_METHOD_TO(QrcodeMgrCtrl::add,    "/admin/api/qrcode/add",         drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(QrcodeMgrCtrl::update, "/admin/api/qrcode/{id}",        drogon::Put,    "AdminAuthFilter");
        ADD_METHOD_TO(QrcodeMgrCtrl::remove, "/admin/api/qrcode/{id}",        drogon::Delete, "AdminAuthFilter");
        ADD_METHOD_TO(QrcodeMgrCtrl::toggle, "/admin/api/qrcode/toggle/{id}", drogon::Post,   "AdminAuthFilter");
        ADD_METHOD_TO(QrcodeMgrCtrl::monitorStatus, "/admin/api/qrcode/monitor/status", drogon::Get, "AdminAuthFilter");
        ADD_METHOD_TO(QrcodeMgrCtrl::monitorSave, "/admin/api/qrcode/monitor/save", drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(QrcodeMgrCtrl::monitorResetKey, "/admin/api/qrcode/monitor/resetKey", drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(QrcodeMgrCtrl::renderQr, "/admin/api/qrcode/png", drogon::Get, "AdminAuthFilter");
        ADD_METHOD_TO(QrcodeMgrCtrl::parseImg, "/admin/api/qrcode/parse", drogon::Post, "AdminAuthFilter");
        // 旧路由兼容
        ADD_METHOD_TO(QrcodeMgrCtrl::list,   "/api/qrcode/list",        drogon::Get,    "AdminAuthFilter");
    METHOD_LIST_END

    void list(const drogon::HttpRequestPtr &req,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string type = req->getParameter("type");
        std::string pluginCode = req->getParameter("plugin_code");
        std::string channelId = req->getParameter("channel_id");
        std::string account = req->getParameter("account");
        std::string sql = "SELECT * FROM pay_qrcode";
        std::vector<std::string> params;
        std::string where;
        if (!type.empty()) { where += (where.empty() ? "" : " AND ") + std::string("type=?"); params.push_back(type); }
        if (!pluginCode.empty()) { where += (where.empty() ? "" : " AND ") + std::string("plugin_code=?"); params.push_back(pluginCode); }
        if (!channelId.empty()) { where += (where.empty() ? "" : " AND ") + std::string("channel_id=?"); params.push_back(channelId); }
        if (!account.empty()) { where += (where.empty() ? "" : " AND ") + std::string("account=?"); params.push_back(account); }
        if (!where.empty()) sql += " WHERE " + where;
        sql += " ORDER BY id DESC";

        auto rows = PayDb::instance().query(sql, params);
        Json::Value items(Json::arrayValue);
        for (auto &row : rows) {
            Json::Value j;
            for (auto &[k, v] : row) {
                if (k == "id" || k == "type" || k == "state" || k == "pattern") {
                    try { j[k] = std::stoi(v); } catch (...) { j[k] = v; }
                } else if (k == "price") {
                    try { j[k] = std::stod(v); } catch (...) { j[k] = v; }
                } else j[k] = v;
            }
            items.append(j);
        }
        Json::Value data; data["items"] = items; data["total"] = (int)rows.size();
        RESP_OK(cb, data);
    }

    void add(const drogon::HttpRequestPtr &req,
             std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; }
        std::string payUrl  = (*body).get("pay_url", "").asString();
        std::string price   = (*body).get("price",   "0").asString();
        if (price.empty()) price = "0";  // 空=任意金额，统一存0
        std::string type    = (*body).get("type",    "1").asString();
        std::string account = (*body).get("account", "").asString();
        std::string pluginCode = (*body).get("plugin_code", "vmq").asString();
        std::string channelId = (*body).get("channel_id", "0").asString();
        std::string qrName = (*body).get("qr_name", "").asString();
        std::string remark = (*body).get("remark", "").asString();
        int pattern         = (*body).get("pattern", 1).asInt();
        if (payUrl.empty()) { RESP_ERR(cb, "收款码 URL 不能为空"); return; }
        if (type != "1" && type != "2" && type != "3") { RESP_ERR(cb, "类型错误"); return; }

        bool ok = PayDb::instance().exec(
            "INSERT INTO pay_qrcode(pay_url,price,type,state,account,pattern,plugin_code,channel_id,qr_name,remark) "
            "VALUES(?,?,?,0,?,?,?,?,?,?)",
            {payUrl, price, type, account, std::to_string(pattern), pluginCode, channelId, qrName, remark});
        if (!ok) { RESP_ERR(cb, "添加失败"); return; }
        RESP_MSG(cb, "添加成功");
    }

    void update(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                std::string id) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; }
        auto &db = PayDb::instance();
        auto row = db.queryOne("SELECT id FROM pay_qrcode WHERE id=?", {id});
        if (row.empty()) { RESP_ERR(cb, "收款码不存在"); return; }

        std::string payUrl  = (*body).get("pay_url", "").asString();
        std::string price   = (*body).get("price",   "0").asString();
        std::string type    = (*body).get("type",    "1").asString();
        std::string account = (*body).get("account", "").asString();
        std::string pluginCode = (*body).get("plugin_code", "vmq").asString();
        std::string channelId = (*body).get("channel_id", "0").asString();
        std::string qrName = (*body).get("qr_name", "").asString();
        std::string remark = (*body).get("remark", "").asString();
        int pattern         = (*body).get("pattern", 1).asInt();

        db.exec(
            "UPDATE pay_qrcode SET pay_url=?,price=?,type=?,account=?,pattern=?,plugin_code=?,channel_id=?,qr_name=?,remark=? "
            "WHERE id=?",
            {payUrl, price, type, account, std::to_string(pattern), pluginCode, channelId, qrName, remark, id});
        RESP_MSG(cb, "更新成功");
    }

    void remove(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                std::string id) {
        PayDb::instance().exec("DELETE FROM pay_qrcode WHERE id=?", {id});
        RESP_MSG(cb, "删除成功");
    }

    void toggle(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                std::string id) {
        auto &db = PayDb::instance();
        auto row = db.queryOne("SELECT state FROM pay_qrcode WHERE id=?", {id});
        if (row.empty()) { RESP_ERR(cb, "收款码不存在"); return; }
        std::string newState = (row["state"] == "0") ? "1" : "0";
        db.exec("UPDATE pay_qrcode SET state=? WHERE id=?", {newState, id});
        RESP_MSG(cb, newState == "0" ? "已启用" : "已禁用");
    }

    void monitorStatus(const drogon::HttpRequestPtr &req,
                       std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto &db = PayDb::instance();
        long long now = std::time(nullptr);
        int onlineSeconds = toInt(db.getSetting("monitor_online_seconds", "180"), 180);
        if (onlineSeconds <= 0) onlineSeconds = 180;
        // 监听端配置地址: 使用 "前端访问的域名" + "后端实际监听端口"
        //  - 域名取自 Origin/X-Forwarded-Host/Referer（前端用户当前访问的 hostname）
        //  - 端口取自后端本地监听端口（监听 App 真正要连接的端口）
        std::string baseUrl = frontendHostWithBackendPort(req);
        if (baseUrl.empty()) baseUrl = requestBaseUrl(req);
        std::string appName = db.getSetting("monitor_app_name", "vmq");

        std::string mchIdStr = req->getParameter("mch_id");
        int mchId = 0; try { mchId = std::stoi(mchIdStr); } catch (...) {}
        std::string pluginCode = req->getParameter("plugin_code");
        if (pluginCode.empty()) pluginCode = "vmq";

        long long lastHeart = 0, lastPay = 0;
        std::string key;     // 监听用密钥(签名 key)
        std::string mchNo;   // 码支付的 pid (商户号)
        std::string scope = "platform";
        if (mchId > 0) {
            auto m = db.queryOne(
                "SELECT id,mch_no,mch_key,vmq_key,"
                "vmq_last_heart,vmq_last_pay,"
                "codepay_last_heart,codepay_last_pay FROM merchant WHERE id=?",
                {std::to_string(mchId)});
            if (m.empty()) { RESP_ERR(cb, "商户不存在"); return; }
            mchNo = m.count("mch_no") ? m["mch_no"] : "";
            if (pluginCode == "codepay") {
                key = m.count("mch_key") ? m["mch_key"] : "";
                lastHeart = toLong(m.count("codepay_last_heart") ? m["codepay_last_heart"] : "0");
                lastPay = toLong(m.count("codepay_last_pay") ? m["codepay_last_pay"] : "0");
            } else {
                key = m["vmq_key"];
                if (key.empty()) {
                    key = randomHex(32);
                    db.exec("UPDATE merchant SET vmq_key=? WHERE id=?", {key, std::to_string(mchId)});
                }
                lastHeart = toLong(m["vmq_last_heart"]);
                lastPay = toLong(m["vmq_last_pay"]);
            }
            scope = "merchant";
        } else {
            key = db.getSetting("key", "");
            if (key.empty()) { key = randomHex(32); db.setSetting("key", key); }
            if (pluginCode == "codepay") {
                lastHeart = toLong(db.getSetting("codepay_lastheart", "0"));
                lastPay   = toLong(db.getSetting("codepay_lastpay",   "0"));
                mchNo = "0"; // 平台级取单 pid=0
            } else {
                lastHeart = toLong(db.getSetting("lastheart", "0"));
                lastPay   = toLong(db.getSetting("lastpay",   "0"));
            }
        }

        bool online = lastHeart > 0 && now - lastHeart <= onlineSeconds;
        if (mchId > 0) {
            const char *col = pluginCode == "codepay" ? "codepay_state" : "vmq_state";
            db.exec(std::string("UPDATE merchant SET ") + col + "=? WHERE id=?",
                    {std::to_string(online ? 1 : 0), std::to_string(mchId)});
        } else {
            std::string stateKey = pluginCode == "codepay" ? "codepay_jkstate" : "jkstate";
            if (!online && db.getSetting(stateKey, "0") == "1") db.setSetting(stateKey, "0");
        }

        // 监听端配置串（按插件协议区分）
        std::string hostOnly = baseUrl;
        auto protoPos = hostOnly.find("://");
        if (protoPos != std::string::npos) hostOnly = hostOnly.substr(protoPos + 3);
        while (!hostOnly.empty() && hostOnly.back() == '/') hostOnly.pop_back();
        // 监听端模式: tenant=0 平台共享(默认), 1=商户独立路径
        std::string tenantMode = req->getParameter("tenant_mode");
        if (tenantMode.empty()) tenantMode = "0";

        std::string configText;
        if (pluginCode == "vmq") {
            // V免签 App 强制 split("/") 取第 2 段为 key, 不支持多段路径
            // 不论 tenantMode 都只输出 host:port/{key}
            // 多租户隔离通过 backend 在 vmq_key 匹配后自动识别 mch_id 实现
            configText = hostOnly + "/" + key;
        } else if (pluginCode == "codepay") {
            // mpay/WePay App: AES-GCM 加密 {"s":host,"p":pid,"k":key} -> "MPAYQR1:..."
            // 与 mpay1.2.4 UserController.php 算法完全一致
            std::string payload = std::string("{\"s\":\"") + hostOnly + "\",\"p\":\"" +
                (mchNo.empty() ? "0" : mchNo) + "\",\"k\":\"" + key + "\"}";
            configText = AesGcmUtils::encryptMpayQr(
                payload, "WePay-Mpay-QR-v1-SharedKey-DoNotChange");
            if (configText.empty()) {
                // 加密失败兜底: 明文逗号分隔
                configText = "mpay," + baseUrl + "," +
                    (mchNo.empty() ? "0" : mchNo) + "," + key;
            }
        } else {
            // V免签 App: host:port/key
            configText = hostOnly + "/" + key;
        }
        Json::Value data;
        data["scope"] = scope;
        data["mch_id"] = mchId;
        data["online"] = online;
        data["state_text"] = online ? "运行正常" : "监听离线";
        data["last_heart"] = (Json::Int64)lastHeart;
        data["last_pay"] = (Json::Int64)lastPay;
        data["server_now"] = (Json::Int64)now;
        data["online_seconds"] = onlineSeconds;
        data["seconds_since_heart"] = lastHeart > 0 ? (Json::Int64)(now - lastHeart) : (Json::Int64)-1;
        data["config_text"] = configText;
        data["qrcode_text"] = configText;
        data["server_url"] = baseUrl;
        data["key"] = key;
        data["app_name"] = appName;
        data["android_download_url"] = db.getSetting("monitor_android_download_url", "");
        data["ios_download_url"] = db.getSetting("monitor_ios_download_url", "");
        RESP_OK(cb, data);
    }

    void monitorSave(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; }
        auto &db = PayDb::instance();
        int mchId = (*body).get("mch_id", 0).asInt();
        for (const auto &k : body->getMemberNames()) {
            if (k == "site_url" || k == "monitor_app_name" ||
                k == "monitor_online_seconds" || k == "monitor_android_download_url" ||
                k == "monitor_ios_download_url") {
                db.setSetting(k, (*body)[k].asString());
            } else if (k == "key") {
                std::string val = (*body)[k].asString();
                if (mchId > 0) db.exec("UPDATE merchant SET vmq_key=? WHERE id=?", {val, std::to_string(mchId)});
                else db.setSetting("key", val);
            }
        }
        RESP_MSG(cb, "监控配置已保存");
    }

    void renderQr(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string text = req->getParameter("text");
        std::string sizeStr = req->getParameter("size");
        int boxSize = 6;
        try { if (!sizeStr.empty()) boxSize = std::max(2, std::min(20, std::stoi(sizeStr) / 33)); } catch (...) {}
        if (text.empty()) { auto r = drogon::HttpResponse::newHttpResponse(); r->setStatusCode(drogon::k400BadRequest); r->setBody("text required"); cb(r); return; }
        QRcode *qr = QRcode_encodeString(text.c_str(), 0, QR_ECLEVEL_M, QR_MODE_8, 1);
        if (!qr) { auto r = drogon::HttpResponse::newHttpResponse(); r->setStatusCode(drogon::k500InternalServerError); r->setBody("encode failed"); cb(r); return; }
        int w = qr->width;
        int margin = 2;
        int dim = (w + margin * 2) * boxSize;
        std::ostringstream svg;
        svg << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << dim << "\" height=\"" << dim
            << "\" viewBox=\"0 0 " << dim << " " << dim << "\" shape-rendering=\"crispEdges\">"
            << "<rect width=\"100%\" height=\"100%\" fill=\"#ffffff\"/><path fill=\"#000000\" d=\"";
        for (int y = 0; y < w; ++y) {
            for (int x = 0; x < w; ++x) {
                if (qr->data[y * w + x] & 1) {
                    int px = (x + margin) * boxSize;
                    int py = (y + margin) * boxSize;
                    svg << "M" << px << " " << py << "h" << boxSize << "v" << boxSize << "h-" << boxSize << "z";
                }
            }
        }
        svg << "\"/></svg>";
        QRcode_free(qr);
        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setBody(svg.str());
        resp->setContentTypeCode(drogon::CT_NONE);
        resp->removeHeader("content-type");
        resp->addHeader("Content-Type", "image/svg+xml; charset=utf-8");
        resp->addHeader("Cache-Control", "no-store");
        resp->addHeader("Access-Control-Allow-Origin", "*");
        cb(resp);
    }

    void parseImg(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        // V免签兼容: 上传二维码图片识别 pay_url
        drogon::MultiPartParser parser;
        if (parser.parse(req) != 0 || parser.getFiles().empty()) {
            RESP_ERR(cb, "请上传二维码图片"); return;
        }
        const auto &file = parser.getFiles()[0];
        const auto &raw = file.fileContent();
        int w = 0, h = 0, ch = 0;
        unsigned char *pixels = stbi_load_from_memory(
            reinterpret_cast<const stbi_uc *>(raw.data()),
            static_cast<int>(raw.size()), &w, &h, &ch, 1);
        if (!pixels) { RESP_ERR(cb, std::string("图片解码失败: ") + (stbi_failure_reason() ? stbi_failure_reason() : "")); return; }
        ZXing::ImageView iv(pixels, w, h, ZXing::ImageFormat::Lum);
        ZXing::ReaderOptions opts;
        opts.setFormats(ZXing::BarcodeFormat::QRCode | ZXing::BarcodeFormat::MicroQRCode);
        opts.setTryHarder(true);
        opts.setTryRotate(true);
        opts.setTryInvert(true);
        auto results = ZXing::ReadBarcodes(iv, opts);
        std::string text;
        for (auto &r : results) { if (r.isValid()) { text = r.text(); break; } }
        stbi_image_free(pixels);
        if (text.empty()) { RESP_ERR(cb, "未识别到二维码"); return; }
        Json::Value data; data["pay_url"] = text; data["url"] = text; RESP_OK(cb, data);
    }

    void monitorResetKey(const drogon::HttpRequestPtr &req,
                        std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        int mchId = body ? (*body).get("mch_id", 0).asInt() : 0;
        std::string key = randomHex(32);
        auto &db = PayDb::instance();
        if (mchId > 0) db.exec("UPDATE merchant SET vmq_key=? WHERE id=?", {key, std::to_string(mchId)});
        else db.setSetting("key", key);
        Json::Value data; data["key"] = key; data["mch_id"] = mchId;
        RESP_OK(cb, data);
    }

private:
    static long long toLong(const std::string &s) { try { return std::stoll(s); } catch (...) { return 0; } }
    static int toInt(const std::string &s, int def) { try { return std::stoi(s); } catch (...) { return def; } }
    static std::string requestBaseUrl(const drogon::HttpRequestPtr &req) {
        std::string host = req->getHeader("host");
        if (host.empty()) host = "127.0.0.1";
        std::string proto = req->getHeader("x-forwarded-proto");
        if (proto.empty()) proto = "http";
        return proto + "://" + host;
    }
    // 组合: 前端访问域名 + 后端实际监听端口
    static std::string frontendHostWithBackendPort(const drogon::HttpRequestPtr &req) {
        // 1. 提取前端 hostname（去端口）
        std::string hostSource;
        std::string origin = req->getHeader("origin");
        if (!origin.empty()) hostSource = origin;
        else {
            std::string xfHost = req->getHeader("x-forwarded-host");
            if (!xfHost.empty()) hostSource = "http://" + xfHost;
            else {
                std::string referer = req->getHeader("referer");
                if (!referer.empty()) hostSource = referer;
            }
        }
        if (hostSource.empty()) return "";
        // 解析 scheme 与 hostname
        std::string scheme = "http";
        auto sp = hostSource.find("://");
        std::string rest = hostSource;
        if (sp != std::string::npos) { scheme = hostSource.substr(0, sp); rest = hostSource.substr(sp + 3); }
        auto slash = rest.find('/'); if (slash != std::string::npos) rest = rest.substr(0, slash);
        std::string hostname = rest;
        auto colon = rest.find(':'); if (colon != std::string::npos) hostname = rest.substr(0, colon);
        // 2. 取后端实际监听端口
        auto local = req->localAddr();
        uint16_t port = local.toPort();
        bool isDefault = (scheme == "http" && port == 80) || (scheme == "https" && port == 443);
        if (isDefault) return scheme + "://" + hostname;
        return scheme + "://" + hostname + ":" + std::to_string(port);
    }
    static std::string randomHex(int n) {
        static const char *cs = "0123456789abcdef";
        std::string s;
        unsigned seed = (unsigned)std::time(nullptr);
        for (int i = 0; i < n; ++i) {
            seed = seed * 1103515245 + 12345;
            s.push_back(cs[(seed >> 16) & 15]);
        }
        return s;
    }
};
