// WePay-Cpp — 打印控制器
// 职责：支持本地热敏打印机和云打印机的打印功能
//
// 支持的打印方式：
// 1. 本地热敏打印机：生成 ESC/POS 指令流，供 PC 挂机软件调用
// 2. 云打印机：转发到飞鹅云 API，支付成功后自动打印小票
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <json/json.h> // JSON 库
#include <string> // 字符串库
#include <sstream> // 字符串流库
#include <ctime> // C 时间库
#include <vector> // 向量库
#include <algorithm> // 算法库
#include <iomanip> // 输入输出格式化库
#include <chrono> // 时间库
#include <openssl/hmac.h> // OpenSSL HMAC 库
#include <openssl/sha.h> // OpenSSL SHA 库
#include <openssl/evp.h> // OpenSSL EVP 库
#include <drogon/HttpClient.h> // Drogon HTTP 客户端
#include "../common/PayDb.h" // 数据库操作
#include "../filters/AdminAuthFilter.h" // 管理员认证过滤器

// 使用 Drogon 命名空间
using namespace drogon;

// ════════════════════════════════════════════════════════════════
// ESC/POS 指令生成器（58mm 热敏打印机标准）
// 用于生成热敏打印机的控制指令流
// ════════════════════════════════════════════════════════════════
class EscPosBuilder {
public:
    // ESC 转义字符（0x1B）
    static constexpr char ESC = 0x1B;
    // GS 组分隔符（0x1D）
    static constexpr char GS  = 0x1D;
    // LF 换行符（0x0A）
    static constexpr char LF  = 0x0A;

    // 存储生成的 ESC/POS 指令字节流
    std::vector<char> buf;

    // 初始化打印机：发送 ESC @ 指令重置打印机状态
    EscPosBuilder& init() {
        // 添加 ESC 字符
        buf.push_back(ESC);
        // 添加 0x40 指令码（重置打印机）
        buf.push_back(0x40);
        // 返回 this 支持链式调用
        return *this;
    }

    // 设置文本居中对齐
    EscPosBuilder& alignCenter() {
        // 添加 ESC 字符
        buf.push_back(ESC);
        // 添加 0x61 指令码（对齐设置）
        buf.push_back(0x61);
        // 添加参数 1（居中）
        buf.push_back(1);
        // 返回 this 支持链式调用
        return *this;
    }
    // 设置文本左对齐
    EscPosBuilder& alignLeft() {
        // 添加 ESC 字符
        buf.push_back(ESC);
        // 添加 0x61 指令码（对齐设置）
        buf.push_back(0x61);
        // 添加参数 0（左对齐）
        buf.push_back(0);
        // 返回 this 支持链式调用
        return *this;
    }
    // 设置文本右对齐
    EscPosBuilder& alignRight() {
        // 添加 ESC 字符
        buf.push_back(ESC);
        // 添加 0x61 指令码（对齐设置）
        buf.push_back(0x61);
        // 添加参数 2（右对齐）
        buf.push_back(2);
        // 返回 this 支持链式调用
        return *this;
    }
    // 启用粗体模式
    EscPosBuilder& boldOn()  {
        // 添加 ESC 字符
        buf.push_back(ESC);
        // 添加 0x45 指令码（粗体设置）
        buf.push_back(0x45);
        // 添加参数 1（启用粗体）
        buf.push_back(1);
        // 返回 this 支持链式调用
        return *this;
    }
    // 禁用粗体模式
    EscPosBuilder& boldOff() {
        // 添加 ESC 字符
        buf.push_back(ESC);
        // 添加 0x45 指令码（粗体设置）
        buf.push_back(0x45);
        // 添加参数 0（禁用粗体）
        buf.push_back(0);
        // 返回 this 支持链式调用
        return *this;
    }

    // 设置字体大小
    // 参数 size：0=正常 1=倍高 2=倍宽 3=倍高宽
    EscPosBuilder& fontSize(int size) {
        // 提取低 2 位作为宽度倍数（0-3）
        int n = size & 0x03;
        // 提取高 2 位作为高度倍数（0-3）
        int m = (size >> 2) & 0x03;
        // 添加 GS 字符
        buf.push_back(GS);
        // 添加 0x21 指令码（字体大小设置）
        buf.push_back(0x21);
        // 添加参数：高度倍数左移 4 位 + 宽度倍数
        buf.push_back((m << 4) | n);
        // 返回 this 支持链式调用
        return *this;
    }

    // 添加一个换行符
    EscPosBuilder& nl()    {
        // 添加 LF 换行符
        buf.push_back(LF);
        // 返回 this 支持链式调用
        return *this;
    }
    // 添加多个换行符
    EscPosBuilder& nls(int n) {
        // 循环 n 次
        for (int i = 0; i < n; ++i)
            // 每次调用 nl() 添加一个换行符
            nl();
        // 返回 this 支持链式调用
        return *this;
    }

    // 添加文本字符串
    EscPosBuilder& text(const std::string& s) {
        // 遍历字符串中的每个字符
        for (char c : s)
            // 将字符添加到缓冲区
            buf.push_back(c);
        // 返回 this 支持链式调用
        return *this;
    }

    // 添加分隔线
    EscPosBuilder& divider(char ch = '-', int n = 32) {
        // 循环 n 次
        for (int i = 0; i < n; ++i)
            // 添加分隔符字符
            buf.push_back(ch);
        // 添加换行符
        nl();
        // 返回 this 支持链式调用
        return *this;
    }

    // 添加左右对齐的行项目：左字符串 + 填充空格 + 右字符串，保持总宽度为 width
    EscPosBuilder& lineItem(const std::string& left, const std::string& right, int width = 32) {
        // 计算左字符串的 UTF-8 字符长度
        int leftLen = utf8Len(left);
        // 计算右字符串的 UTF-8 字符长度
        int rightLen = utf8Len(right);
        // 计算总长度
        int total = leftLen + rightLen;
        // 如果总长度小于指定宽度
        if (total < width) {
            // 添加左字符串 + 填充空格 + 右字符串
            text(left + std::string(width - total, ' ') + right);
        } else {
            // 否则直接添加左右字符串（不填充）
            text(left + right);
        }
        // 添加换行符
        nl();
        // 返回 this 支持链式调用
        return *this;
    }

    // 切纸指令
    EscPosBuilder& cut() {
        // 添加 GS 字符
        buf.push_back(GS);
        // 添加 0x56 指令码（切纸）
        buf.push_back(0x56);
        // 添加参数 0x00（全切）
        buf.push_back(0x00);
        // 返回 this 支持链式调用
        return *this;
    }

    // 打开钱箱指令
    EscPosBuilder& openCashDrawer() {
        // 添加 ESC 字符
        buf.push_back(ESC);
        // 添加 0x70 指令码（钱箱控制）
        buf.push_back(0x70);
        // 添加参数 0x00（模式）
        buf.push_back(0x00);
        // 添加参数 0x19（脉冲宽度）
        buf.push_back(0x19);
        // 添加参数 0xFA（脉冲间隔）
        buf.push_back(0xFA);
        return *this;
    }

    std::string toBase64() const {
        std::string raw(buf.begin(), buf.end());
        return base64Encode(raw);
    }

    // UTF-8 字符串显示宽度（中文=2，ASCII=1）
    static int utf8Len(const std::string& s) {
        int len = 0;
        for (size_t i = 0; i < s.size(); ) {
            unsigned char c = s[i];
            if (c < 0x80) { len += 1; ++i; }
            else if ((c & 0xE0) == 0xC0) { len += 2; i += 2; }
            else if ((c & 0xF0) == 0xE0) { len += 2; i += 3; }
            else { len += 2; i += 4; }
        }
        return len;
    }

    // 简单 Base64（不含 std::codecvt，避免跨平台问题）
    static std::string base64Encode(const std::string& input) {
        static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        int i = 0;
        const unsigned char* d = (const unsigned char*)input.data();
        int len = (int)input.size();
        while (i < len) {
            int b0 = d[i++];
            int b1 = (i < len) ? d[i++] : 0;
            int b2 = (i < len) ? d[i++] : 0;
            out.push_back(tbl[b0 >> 2]);
            out.push_back(tbl[((b0 & 0x03) << 4) | (b1 >> 4)]);
            out.push_back(i > len + 1 ? '=' : tbl[((b1 & 0x0F) << 2) | (b2 >> 6)]);
            out.push_back(i > len ? '=' : tbl[b2 & 0x3F]);
        }
        return out;
    }
};

// ════════════════════════════════════════════════════════════════
// 打印控制器
// ════════════════════════════════════════════════════════════════
static std::string nowStr() {
    char buf[64];
    std::time_t t = std::time(nullptr);
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return std::string(buf);
}
class PrintCtrl : public HttpController<PrintCtrl> {
public:
    METHOD_LIST_BEGIN
        // 生成小票 ESC/POS 指令（方案 B）
        ADD_METHOD_TO(PrintCtrl::generateReceipt, "/admin/api/print/receipt", drogon::Get, "AdminAuthFilter");
        ADD_METHOD_TO(PrintCtrl::generateReceiptPost, "/admin/api/print/receipt", drogon::Post, "AdminAuthFilter");

        // 云打印（方案 C: 飞鹅云）
        ADD_METHOD_TO(PrintCtrl::cloudPrint, "/admin/api/print/cloud", drogon::Post, "AdminAuthFilter");

        // 云打印状态查询
        ADD_METHOD_TO(PrintCtrl::cloudPrintStatus, "/admin/api/print/cloud/status", drogon::Get, "AdminAuthFilter");

        // 打印机配置管理
        ADD_METHOD_TO(PrintCtrl::getPrinterConfig, "/admin/api/print/config", drogon::Get, "AdminAuthFilter");
        ADD_METHOD_TO(PrintCtrl::savePrinterConfig, "/admin/api/print/config", drogon::Post, "AdminAuthFilter");

        // 打印机 CRUD
        ADD_METHOD_TO(PrintCtrl::listPrinters, "/admin/api/print/printers", drogon::Get, "AdminAuthFilter");
        ADD_METHOD_TO(PrintCtrl::addPrinter, "/admin/api/print/printers", drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(PrintCtrl::editPrinter, "/admin/api/print/printers/{1}", drogon::Put, "AdminAuthFilter");
        ADD_METHOD_TO(PrintCtrl::delPrinter, "/admin/api/print/printers/{1}", drogon::Delete, "AdminAuthFilter");

        // 打印日志
        ADD_METHOD_TO(PrintCtrl::listPrintLogs, "/admin/api/print/logs", drogon::Get, "AdminAuthFilter");
    METHOD_LIST_END

    // ══════════════════════════════════════════════════════════
    // 生成收款小票（ESC/POS 指令流）
    // GET /admin/api/print/receipt?order_id=xxx
    // 返回: application/octet-stream (.bin 文件) 或 application/json (Base64)
    // ══════════════════════════════════════════════════════════
    void generateReceipt(const HttpRequestPtr &req,
                         std::function<void(const HttpResponsePtr &)> &&cb) {
        auto orderId = req->getParameter("order_id");
        auto format = req->getParameter("format"); // "bin" | "base64" (默认 bin)

        if (orderId.empty()) {
            Json::Value r; r["code"] = -1; r["msg"] = "order_id 不能为空";
            cb(HttpResponse::newHttpJsonResponse(r)); return;
        }

        auto receipt = buildReceiptData(orderId, PayDb::instance());
        if (receipt.empty()) {
            Json::Value r; r["code"] = -1; r["msg"] = "订单不存在或未支付";
            cb(HttpResponse::newHttpJsonResponse(r)); return;
        }

        EscPosBuilder b;
        buildReceiptEscPos(receipt, b);

        if (format == "base64") {
            Json::Value r; r["code"] = 0;
            r["data"]["order_id"] = orderId;
            r["data"]["base64"] = b.toBase64();
            r["data"]["size_bytes"] = (Json::Int64)b.buf.size();
            cb(HttpResponse::newHttpJsonResponse(r));
        } else {
            // 返回二进制流
            std::string raw(b.buf.begin(), b.buf.end());
            auto resp = HttpResponse::newHttpResponse();
            resp->setContentTypeCode(ContentType::CT_APPLICATION_OCTET_STREAM);
            std::ostringstream oss;
            oss << "attachment; filename=\"receipt_" << orderId << ".bin\"";
            resp->addHeader("Content-Disposition", oss.str());
            resp->addHeader("Content-Length", std::to_string(raw.size()));
            resp->setBody(raw);
            cb(resp);
        }
    }

    void generateReceiptPost(const HttpRequestPtr &req,
                             std::function<void(const HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) {
            Json::Value r; r["code"] = -1; r["msg"] = "参数格式错误";
            cb(HttpResponse::newHttpJsonResponse(r)); return;
        }
        auto orderId = (*body).get("order_id", "").asString();
        if (orderId.empty()) {
            Json::Value r; r["code"] = -1; r["msg"] = "order_id 不能为空";
            cb(HttpResponse::newHttpJsonResponse(r)); return;
        }
        auto receipt = buildReceiptData(orderId, PayDb::instance());
        if (receipt.empty()) {
            Json::Value r; r["code"] = -1; r["msg"] = "订单不存在或未支付";
            cb(HttpResponse::newHttpJsonResponse(r)); return;
        }
        EscPosBuilder b;
        buildReceiptEscPos(receipt, b);
        Json::Value r; r["code"] = 0;
        r["data"]["order_id"] = orderId;
        r["data"]["base64"] = b.toBase64();
        r["data"]["size_bytes"] = (Json::Int64)b.buf.size();
        cb(HttpResponse::newHttpJsonResponse(r));
    }

    // ══════════════════════════════════════════════════════════
    // 云打印（转发到飞鹅云 API）
    // POST /admin/api/print/cloud
    // body: { printer_sn, printer_key, content, copies, order_id }
    // ══════════════════════════════════════════════════════════
    void cloudPrint(const HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) {
            Json::Value r; r["code"] = -1; r["msg"] = "参数格式错误";
            cb(HttpResponse::newHttpJsonResponse(r)); return;
        }
        auto &j = *body;
        std::string printerSn = j.get("printer_sn", "").asString();
        std::string printerKey = j.get("printer_key", "").asString();
        std::string content = j.get("content", "").asString();
        int copies = j.get("copies", 1).asInt();
        std::string orderId = j.get("order_id", "").asString();

        if (printerSn.empty() || printerKey.empty() || content.empty()) {
            Json::Value r; r["code"] = -1; r["msg"] = "printer_sn / printer_key / content 均不能为空";
            cb(HttpResponse::newHttpJsonResponse(r)); return;
        }

        auto &db = PayDb::instance();
        if (printerKey == "***USE_DB_CONFIG***") {
            printerKey = db.getSetting("feie_printer_key");
        }

        std::string apiUrl = db.getSetting("feie_api_url", "http://api.feieyun.com/Api/Order");
        std::string feieUser = db.getSetting("feie_user");
        std::string stime = std::to_string(std::time(nullptr));

        // 飞鹅云 sig = HMAC-SHA256(UKEY, user+sn+UKEY+stime)
        std::string sigData = feieUser + printerSn + printerKey + stime;
        std::string sig = hmacSha256Hex(printerKey, sigData);

        // 构建飞鹅云请求内容
        std::ostringstream contentoss;
        contentoss << "<script>sz=" << copies << "|sj=" << escapeFeie(content) << ";</script>";

        std::string postData =
            "user=" + urlEncode(feieUser) +
            "&stime=" + stime +
            "&sig=" + sig +
            "&apiname=PrintSchoolNotice" +
            "&sn=" + printerSn +
            "&content=" + urlEncode(contentoss.str()) +
            "&times=" + std::to_string(copies);

        std::string taskId = orderId + "_" + stime;
        long long nowTs = std::time(nullptr);

        // 记录任务（state=0 排队中）
        db.exec(
            "INSERT INTO print_task(task_id,printer_sn,printer_key,content,copies,order_id,state,created_at) "
            "VALUES(?,?,?,?,?,?,0,?)",
            {taskId, printerSn, printerKey, content,
             std::to_string(copies), orderId,
             std::to_string(nowTs)});

        // Drogon 异步 HTTP 客户端调用飞鹅云（非阻塞）
        std::string savedTaskId = taskId;
        std::string savedNow = std::to_string(nowTs);
        std::string feieUrl = apiUrl;

        auto feieClient = HttpClient::newHttpClient(feieUrl);
        auto feieReq = HttpRequest::newHttpRequest();
        feieReq->setMethod(drogon::Post);
        feieReq->setPath("/");
        feieReq->addHeader("Content-Type", "application/x-www-form-urlencoded");
        feieReq->setBody(postData);

        feieClient->sendRequest(feieReq,
            [savedTaskId, savedNow](drogon::ReqResult result, const drogon::HttpResponsePtr &resp) {
                auto &db2 = PayDb::instance();
                long long ts = std::time(nullptr);
                std::string errMsg;
                bool ok = false;
                if (result == drogon::ReqResult::Ok && resp) {
                    auto bodyView = resp->getBody();
                    std::string bodyStr(bodyView.begin(), bodyView.end());
                    if (bodyStr.find("\"ok\":true") != std::string::npos ||
                        bodyStr.find("\"ok\": true") != std::string::npos) {
                        ok = true;
                    } else {
                        errMsg = bodyStr.substr(0, 512);
                    }
                } else {
                    errMsg = std::string("HTTP failed: ") + std::to_string((int)result);
                }
                if (ok) {
                    db2.exec("UPDATE print_task SET state=1,updated_at=? WHERE task_id=?",
                             {std::to_string(ts), savedTaskId});
                } else {
                    db2.exec("UPDATE print_task SET state=-1,error_msg=?,updated_at=? WHERE task_id=?",
                             {errMsg, std::to_string(ts), savedTaskId});
                }
            });

        Json::Value r; r["code"] = 0; r["msg"] = "打印任务已提交";
        r["data"]["task_id"] = taskId;
        r["data"]["printer_sn"] = printerSn;
        cb(HttpResponse::newHttpJsonResponse(r));
    }

    // ══════════════════════════════════════════════════════════
    // 查询云打印任务状态
    // GET /admin/api/print/cloud/status?task_id=xxx
    // ══════════════════════════════════════════════════════════
    void cloudPrintStatus(const drogon::HttpRequestPtr &req,
                          std::function<void(const HttpResponsePtr &)> &&cb) {
        auto taskId = req->getParameter("task_id");
        if (taskId.empty()) {
            Json::Value r; r["code"] = -1; r["msg"] = "task_id 不能为空";
            cb(HttpResponse::newHttpJsonResponse(r)); return;
        }
        auto &db = PayDb::instance();
        auto row = db.queryOne(
            "SELECT state,updated_at,error_msg FROM print_task WHERE task_id=?",
            {taskId});
        Json::Value r; r["code"] = 0;
        if (row.empty()) {
            r["data"]["state"] = -1;
            r["data"]["state_text"] = "任务不存在";
        } else {
            int state = std::stoi(row["state"]);
            r["data"]["state"] = state;
            r["data"]["state_text"] = state == 0 ? "排队中" : (state == 1 ? "已打印" : "打印失败");
            r["data"]["updated_at"] = row["updated_at"];
            auto itErr = row.find("error_msg");
            r["data"]["error_msg"] = (itErr != row.end()) ? itErr->second : "";
        }
        cb(HttpResponse::newHttpJsonResponse(r));
    }

    // ══════════════════════════════════════════════════════════
    // 打印机配置管理
    // ══════════════════════════════════════════════════════════
    void getPrinterConfig(const HttpRequestPtr &req,
                          std::function<void(const HttpResponsePtr &)> &&cb) {
        auto &db = PayDb::instance();
        Json::Value r; r["code"] = 0;
        r["data"]["feie_user"] = db.getSetting("feie_user");
        r["data"]["feie_printer_key"] = db.getSetting("feie_printer_key");
        r["data"]["feie_api_url"] = db.getSetting("feie_api_url", "http://api.feieyun.com/Api/Order");
        r["data"]["default_printer_sn"] = db.getSetting("default_printer_sn");
        r["data"]["print_copies"] = std::stoi(db.getSetting("print_copies", "1"));
        cb(HttpResponse::newHttpJsonResponse(r));
    }

    void savePrinterConfig(const HttpRequestPtr &req,
                            std::function<void(const HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) {
            Json::Value r; r["code"] = -1; r["msg"] = "参数格式错误";
            cb(HttpResponse::newHttpJsonResponse(r)); return;
        }
        auto &j = *body;
        auto &db = PayDb::instance();
        auto setIf = [&](const char* key, const Json::Value& val) {
            if (!val.isNull()) db.setSetting(key, val.asString());
        };
        setIf("feie_user",          j.get("feie_user", Json::Value::nullRef));
        setIf("feie_printer_key",   j.get("feie_printer_key", Json::Value::nullRef));
        setIf("feie_api_url",       j.get("feie_api_url", Json::Value::nullRef));
        setIf("default_printer_sn",  j.get("default_printer_sn", Json::Value::nullRef));
        setIf("print_copies",        j.get("print_copies", Json::Value::nullRef));
        Json::Value r; r["code"] = 0; r["msg"] = "配置已保存";
        cb(HttpResponse::newHttpJsonResponse(r));
    }

    // ══════════════════════════════════════════════════════════
    // 打印机列表
    // GET /admin/api/print/printers
    // ══════════════════════════════════════════════════════════
    void listPrinters(const HttpRequestPtr &req,
                      std::function<void(const HttpResponsePtr &)> &&cb) {
        auto &db = PayDb::instance();
        auto rows = db.query(
            "SELECT id,name,sn,key,brand,remark,state,created_at,updated_at "
            "FROM print_printer ORDER BY id DESC");
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value item;
            item["id"] = std::stoi(r["id"]);
            item["name"] = r["name"];
            item["sn"] = r["sn"];
            item["key"] = "";  // 不返回密钥
            item["brand"] = r["brand"];
            item["remark"] = r["remark"];
            item["state"] = std::stoi(r["state"]);
            item["created_at"] = (Json::Value::Int64)std::stoll(r["created_at"]);
            item["updated_at"] = r.count("updated_at") > 0 ? (Json::Value::Int64)std::stoll(r.at("updated_at")) : 0;
            arr.append(item);
        }
        Json::Value out; out["code"] = 0; out["list"] = arr;
        cb(HttpResponse::newHttpJsonResponse(out));
    }

    // ══════════════════════════════════════════════════════════
    // 添加打印机
    // POST /admin/api/print/printers
    // ══════════════════════════════════════════════════════════
    void addPrinter(const HttpRequestPtr &req,
                    std::function<void(const HttpResponsePtr &)> &&cb) {
        auto body = req->getJsonObject();
        if (!body) { deny(cb, "参数格式错误"); return; }
        auto &j = *body;
        auto name = j.get("name", "").asString();
        auto sn = j.get("sn", "").asString();
        auto key = j.get("key", "").asString();
        auto brand = j.get("brand", "feie").asString();
        auto remark = j.get("remark", "").asString();
        int state = j.get("state", 1).asInt();
        if (name.empty() || sn.empty() || key.empty()) {
            deny(cb, "名称、SN、密钥均不能为空"); return;
        }
        auto &db = PayDb::instance();
        auto exists = db.queryOne("SELECT id FROM print_printer WHERE sn=?", {sn});
        if (!exists.empty()) { deny(cb, "该 SN 已存在"); return; }
        long long now = std::time(nullptr);
        db.exec(
            "INSERT INTO print_printer(name,sn,`key`,brand,remark,state,created_at,updated_at) "
            "VALUES(?,?,?,?,?,?,?,?)",
            {name, sn, key, brand, remark, std::to_string(state),
             std::to_string(now), std::to_string(now)});
        Json::Value r; r["code"] = 0; r["msg"] = "添加成功";
        cb(HttpResponse::newHttpJsonResponse(r));
    }

    // ══════════════════════════════════════════════════════════
    // 编辑打印机
    // PUT /admin/api/print/printers/{id}
    // ══════════════════════════════════════════════════════════
    void editPrinter(const HttpRequestPtr &req,
                     std::function<void(const HttpResponsePtr &)> &&cb,
                     int id) {
        auto body = req->getJsonObject();
        if (!body) { deny(cb, "参数格式错误"); return; }
        auto &j = *body;
        auto &db = PayDb::instance();
        long long now = std::time(nullptr);
        std::string sql = "UPDATE print_printer SET name=?,sn=?,brand=?,remark=?,state=?,updated_at=?";
        std::vector<std::string> params = {
            j.get("name","").asString(),
            j.get("sn","").asString(),
            j.get("brand","feie").asString(),
            j.get("remark","").asString(),
            std::to_string(j.get("state",1).asInt()),
            std::to_string(now)
        };
        // 如果传了新密钥才更新
        if (!j.get("key","").asString().empty()) {
            sql += ",`key`=?";
            params.push_back(j.get("key","").asString());
        }
        sql += " WHERE id=?";
        params.push_back(std::to_string(id));
        db.exec(sql, params);
        Json::Value r; r["code"] = 0; r["msg"] = "更新成功";
        cb(HttpResponse::newHttpJsonResponse(r));
    }

    // ══════════════════════════════════════════════════════════
    // 删除打印机
    // DELETE /admin/api/print/printers/{id}
    // ══════════════════════════════════════════════════════════
    void delPrinter(const HttpRequestPtr &req,
                    std::function<void(const HttpResponsePtr &)> &&cb,
                    int id) {
        auto &db = PayDb::instance();
        db.exec("DELETE FROM print_printer WHERE id=?", {std::to_string(id)});
        Json::Value r; r["code"] = 0; r["msg"] = "删除成功";
        cb(HttpResponse::newHttpJsonResponse(r));
    }

    static int safeInt(const std::string& s, int def) {
        try { return std::stoi(s); } catch (...) { return def; }
    }

    static std::string getParam(const HttpRequestPtr &req, const char* key, const char* def = "") {
        auto v = req->getParameter(key);
        return v.empty() ? std::string(def) : v;
    }

    // ══════════════════════════════════════════════════════════
    // 打印日志
    // GET /admin/api/print/logs
    // ══════════════════════════════════════════════════════════
    void listPrintLogs(const HttpRequestPtr &req,
                      std::function<void(const HttpResponsePtr &)> &&cb) {
        auto &db = PayDb::instance();
        int page = safeInt(getParam(req, "page"), 1);
        int size = safeInt(getParam(req, "size"), 20);
        if (page < 1) page = 1;
        if (size < 1 || size > 100) size = 20;
        int offset = (page - 1) * size;
        auto cnt = db.queryOne("SELECT COUNT(*) AS c FROM print_log");
        int total = cnt.empty() ? 0 : std::stoi(cnt["c"]);
        auto rows = db.query(
            "SELECT id,order_id,print_type,printer_sn,copies,result,error_msg,created_at "
            "FROM print_log ORDER BY id DESC LIMIT ? OFFSET ?",
            {std::to_string(size), std::to_string(offset)});
        Json::Value arr(Json::arrayValue);
        for (auto &r : rows) {
            Json::Value item;
            item["id"] = std::stoi(r["id"]);
            item["order_id"] = r["order_id"];
            item["print_type"] = r["print_type"];
            item["printer_sn"] = r["printer_sn"];
            item["copies"] = std::stoi(r["copies"]);
            item["result"] = r["result"];
            item["error_msg"] = r["error_msg"];
            item["created_at"] = (Json::Value::Int64)std::stoll(r["created_at"]);
            arr.append(item);
        }
        Json::Value out;
        out["code"] = 0;
        out["list"] = arr;
        out["total"] = total;
        out["page"] = page;
        out["size"] = size;
        cb(HttpResponse::newHttpJsonResponse(out));
    }

    static void deny(std::function<void(const HttpResponsePtr &)> &cb, const std::string &msg) {
        Json::Value r; r["code"] = -1; r["msg"] = msg;
        cb(HttpResponse::newHttpJsonResponse(r));
    }

private:
    // ══════════════════════════════════════════════════════════
    // 查询订单+商户信息，填充小票所需字段
    // ══════════════════════════════════════════════════════════
    static Json::Value buildReceiptData(const std::string& orderId, PayDb& db) {
        Json::Value empty;
        auto row = db.queryOne(
            "SELECT o.order_id,o.mch_order_no,o.pay_type,o.amount,o.real_amount,"
            "o.mch_fee_amount,o.subject,o.state,o.pay_time,o.created_at,"
            "m.mch_name,m.mch_no "
            "FROM pay_order o "
            "JOIN merchant m ON m.id=o.mch_id "
            "WHERE o.order_id=?",
            {orderId});
        if (row.empty()) return empty;

        // 手动构建 Json::Value（PayDb 返回的是 unordered_map<string,string>）
        Json::Value out;
        auto get = [&](const std::string& k) -> std::string {
            auto it = row.find(k);
            return (it != row.end()) ? it->second : "";
        };
        out["order_id"]     = get("order_id");
        out["mch_order_no"]  = get("mch_order_no");
        out["pay_type"]      = get("pay_type");
        out["amount"]        = get("amount");
        out["real_amount"]   = get("real_amount");
        out["mch_fee_amount"] = get("mch_fee_amount");
        out["subject"]       = get("subject");
        out["state"]         = get("state");
        out["pay_time"]      = get("pay_time");
        out["created_at"]    = get("created_at");
        out["mch_name"]      = get("mch_name");
        out["mch_no"]        = get("mch_no");
        return out;
    }

    // ══════════════════════════════════════════════════════════
    // 用 EscPosBuilder 生成小票字节流
    // ══════════════════════════════════════════════════════════
    static void buildReceiptEscPos(const Json::Value& row, EscPosBuilder& b) {
        b.init();

        // 标题
        b.alignCenter();
        b.fontSize(2);
        b.boldOn();
        b.text("收款小票");
        b.nl();
        b.fontSize(0);
        b.boldOff();
        b.divider('=', 32);
        b.nl();

        // 商户名
        b.alignLeft();
        b.boldOn();
        b.text(row.isMember("mch_name") && !row["mch_name"].isNull() && !row["mch_name"].asString().empty()
               ? row["mch_name"].asString() : "商户收款");
        b.boldOff();
        b.nl();
        b.divider('-', 32);

        // 订单信息
        b.fontSize(1);
        b.boldOn();
        b.text("订单信息");
        b.boldOff();
        b.nl();
        b.fontSize(0);

        std::string orderId = row["order_id"].asString();
        std::string payType = row["pay_type"].asString();
        std::string amount = row.isMember("real_amount") && !row["real_amount"].isNull() && !row["real_amount"].asString().empty()
                            ? row["real_amount"].asString() : row["amount"].asString();
        std::string payTime;
        if (row.isMember("pay_time") && !row["pay_time"].isNull() && !row["pay_time"].asString().empty()) {
            long long t = std::stoll(row["pay_time"].asString());
            if (t > 0) {
                char buf[64];
                std::time_t tt = (t > 1e12 ? t / 1000 : t);
                std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&tt));
                payTime = buf;
            }
        }
        if (payTime.empty()) payTime = nowStr();

        std::string payTypeName;
        if (payType == "alipay") payTypeName = "支付宝";
        else if (payType == "wxpay") payTypeName = "微信支付";
        else if (payType == "qqpay") payTypeName = "QQ钱包";
        else if (payType == "unionpay") payTypeName = "云闪付";
        else payTypeName = payType;

        b.lineItem("订单号:", orderId);
        if (row.isMember("mch_order_no") && !row["mch_order_no"].isNull() && !row["mch_order_no"].asString().empty())
            b.lineItem("商户单号:", row["mch_order_no"].asString());
        b.lineItem("交易时间:", payTime);
        b.lineItem("支付方式:", payTypeName);
        if (row.isMember("subject") && !row["subject"].isNull() && !row["subject"].asString().empty())
            b.lineItem("商品名称:", row["subject"].asString());
        b.divider('-', 32);

        // 金额（突出显示）
        b.alignCenter();
        b.fontSize(1);
        b.boldOn();
        b.text("实收金额");
        b.nl();
        b.fontSize(3);
        b.text("¥" + amount);
        b.nl();
        b.fontSize(0);
        b.boldOff();
        b.alignLeft();
        b.nl();

        // 手续费
        if (row.isMember("mch_fee_amount") && !row["mch_fee_amount"].isNull()
            && !row["mch_fee_amount"].asString().empty()
            && row["mch_fee_amount"].asString() != "0" && row["mch_fee_amount"].asString() != "0.00") {
            b.divider('-', 32);
            b.lineItem("手续费:", "¥" + row["mch_fee_amount"].asString());
        }

        // 底部
        b.divider('-', 32);
        b.alignCenter();
        b.fontSize(2);
        b.boldOn();
        b.text("=== 支付成功 ===");
        b.boldOff();
        b.nl();
        b.fontSize(0);
        b.text("感谢惠顾，欢迎下次光临");
        b.nls(2);

        b.openCashDrawer();
        b.cut();
    }

    static std::string sha256Hex(const std::string& input) {
        unsigned char hash[SHA256_DIGEST_LENGTH];
        HMAC(EVP_sha256(),
             input.data(), input.size(),
             (unsigned char*)input.data(), input.size(),
             hash, nullptr);
        std::ostringstream oss;
        for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
            oss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
        return oss.str();
    }

    static std::string hmacSha256Hex(const std::string& key,
                                     const std::string& data) {
        unsigned char hash[SHA256_DIGEST_LENGTH];
        HMAC(EVP_sha256(),
             key.data(), key.size(),
             (unsigned char*)data.data(), data.size(),
             hash, nullptr);
        std::ostringstream oss;
        for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
            oss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
        return oss.str();
    }

    static std::string escapeFeie(const std::string& s) {
        std::string r;
        for (char c : s) {
            if (c == '<') r += "&lt;";
            else if (c == '>') r += "&gt;";
            else if (c == '&') r += "&amp;";
            else r += c;
        }
        return r;
    }

    static std::string urlEncode(const std::string& s) {
        static const char* hex = "0123456789ABCDEF";
        std::string r;
        for (unsigned char c : s) {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                r += c;
            } else {
                r += '%';
                r += hex[(c >> 4) & 0x0F];
                r += hex[c & 0x0F];
            }
        }
        return r;
    }

};
