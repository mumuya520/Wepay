// WePay-Cpp — 商户文件上传 (收款码图片)
// POST /merchant/api/upload/qrcode  上传收款码图片 (multipart/form-data, 字段名 file)
// POST /merchant/api/qrcode/save    保存收款码到数据库
// GET  /merchant/api/qrcode/get     查询商户的收款码
// GET  /merchant/api/file/proxy     代理文件访问
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <filesystem> // 文件系统操作
#include <random> // 随机数生成
#include <ctime> // 时间库
#include <algorithm> // 算法库
#include <fstream> // 文件流
#include <sstream> // 字符串流
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/NotifyChannels.h" // OSS 服务（MinIO/阿里云）
#include "../filters/MerchantAuthFilter.h" // 商户认证过滤器
#include "../common/PayDb.h" // 数据库操作

// 商户文件上传控制器类
class MerchantFileUploadCtrl : public drogon::HttpController<MerchantFileUploadCtrl> {
public:
    METHOD_LIST_BEGIN // 路由列表开始
        ADD_METHOD_TO(MerchantFileUploadCtrl::uploadQrcode, "/merchant/api/upload/qrcode",
                      drogon::Post, "MerchantAuthFilter"); // 上传收款码
        ADD_METHOD_TO(MerchantFileUploadCtrl::saveQrcode, "/merchant/api/qrcode/save",
                      drogon::Post, "MerchantAuthFilter"); // 保存收款码
        ADD_METHOD_TO(MerchantFileUploadCtrl::getQrcode, "/merchant/api/qrcode/get",
                      drogon::Get, "MerchantAuthFilter"); // 查询收款码
        ADD_METHOD_TO(MerchantFileUploadCtrl::proxyFile, "/merchant/api/file/proxy",
                      drogon::Get, "MerchantAuthFilter"); // 代理文件
    METHOD_LIST_END // 路由列表结束

    // 上传收款码图片方法
    void uploadQrcode(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                      std::function<void(const drogon::HttpResponsePtr &)> &&cb) // 响应回调函数
    {
        drogon::MultiPartParser parser; // 创建多部分解析器
        if (parser.parse(req) != 0 || parser.getFiles().empty()) { // 解析请求和检查文件
            RESP_ERR(cb, "请上传文件"); return; // 文件为空
        }

        auto &file = parser.getFiles()[0]; // 获取第一个文件
        std::string origName = file.getFileName(); // 获取原始文件名
        std::string ext = getExt(origName); // 获取文件扩展名

        // 只允许图片
        static const std::set<std::string> allowed = { // 允许的图片格式
            ".jpg", ".jpeg", ".png", ".gif", ".webp"
        };
        std::string extLower = ext; // 转换为小写
        std::transform(extLower.begin(), extLower.end(), extLower.begin(), ::tolower); // 转换为小写
        if (allowed.find(extLower) == allowed.end()) { // 检查扩展名是否允许
            RESP_ERR(cb, "不支持的图片格式: " + ext); return; // 不支持的格式
        }

        // 限制大小 5MB
        if (file.fileLength() > 5 * 1024 * 1024) { // 检查文件大小
            RESP_ERR(cb, "文件大小不能超过 5MB"); return; // 文件过大
        }

        // 先保存到本地临时目录
        std::string uploadDir = "./uploads"; // 上传目录
        std::filesystem::create_directories(uploadDir); // 创建目录
        std::string newName = std::to_string(std::time(nullptr)) + "_" + randStr(6) + extLower; // 生成新文件名
        std::string savePath = uploadDir + "/" + newName; // 完整保存路径
        file.saveAs(savePath); // 保存文件

        // 上传到 MinIO（如果配置了）
        std::string keyName = newName;  // 直接上传到根目录
        std::string url = OssService::instance().upload(keyName, savePath); // 上传到 OSS
        if (url.empty()) url = "/uploads/" + newName;  // MinIO 失败则回退本地

        Json::Value data; // 响应数据
        data["url"]      = url; // 文件 URL
        data["filename"] = newName; // 新文件名
        data["size"]     = (Json::UInt64)file.fileLength(); // 文件大小
        RESP_OK(cb, data); // 返回成功响应
    }

    // 保存收款码到数据库方法
    void saveQrcode(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                    std::function<void(const drogon::HttpResponsePtr &)> &&cb) // 响应回调函数
    {
        auto json = req->getJsonObject(); // 获取 JSON 请求体
        if (!json) { RESP_ERR(cb, "参数错误"); return; } // 参数错误

        std::string url = (*json)["url"].asString(); // 获取文件 URL
        std::string payType = (*json)["pay_type"].asString(); // 获取支付类型
        if (url.empty() || payType.empty()) { // 检查参数
            RESP_ERR(cb, "url 和 pay_type 不能为空"); return; // 参数不完整
        }

        // 获取商户 ID（从 X-Mch-Id 头）
        std::string mchIdStr = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        if (mchIdStr.empty()) { RESP_ERR(cb, "获取商户 ID 失败"); return; } // 商户 ID 获取失败
        int mchId = std::stoi(mchIdStr); // 转换为整数

        // 保存到 pay_qrcode 表
        auto &db = PayDb::instance(); // 获取数据库单例
        try {
            int type = payType == "wxpay" ? 1 : (payType == "alipay" ? 2 : 3); // 支付类型映射
            std::string qrName = "merchant_upload_" + std::to_string(std::time(nullptr)); // 生成二维码名称
            std::string now = std::to_string(std::time(nullptr)); // 获取当前时间戳
            
            db.exec( // 插入二维码记录
                "INSERT INTO pay_qrcode (mch_id, type, pay_url, state, created_at, plugin_code, qr_name) "
                "VALUES (?, ?, ?, 0, ?, 'wepay_merchant', ?)",
                {std::to_string(mchId), std::to_string(type), url, now, qrName}
            );
            
            Json::Value data; // 响应数据
            data["url"] = url; // 返回文件 URL
            RESP_OK(cb, data); // 返回成功响应
        } catch (const std::exception &e) { // 捕获异常
            RESP_ERR(cb, std::string("数据库错误: ") + e.what()); // 返回错误
        }
    }

    // 查询商户的收款码方法
    void getQrcode(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) // 响应回调函数
    {
        // 获取商户 ID（从 X-Mch-Id 头）
        std::string mchIdStr = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        if (mchIdStr.empty()) { RESP_ERR(cb, "获取商户 ID 失败"); return; } // 商户 ID 获取失败
        int mchId = std::stoi(mchIdStr); // 转换为整数

        // 查询最新的收款码
        auto &db = PayDb::instance(); // 获取数据库单例
        try {
            auto rows = db.query( // 查询二维码
                "SELECT id, pay_url, type, created_at FROM pay_qrcode " // 查询二维码信息
                "WHERE mch_id = ? " // 按商户 ID 查询
                "ORDER BY created_at DESC LIMIT 1", // 按创建时间倒序，取最新一条
                {std::to_string(mchId)}
            );
            
            if (!rows.empty()) { // 如果有结果
                auto &row = rows[0]; // 获取第一条记录
                Json::Value data; // 响应数据
                try {
                    data["id"] = Json::Value::Int64(std::stoll(row.at("id"))); // 二维码 ID
                    data["url"] = row.at("pay_url"); // 二维码 URL
                    data["type"] = std::stoi(row.at("type")); // 支付类型
                    data["created_at"] = Json::Value::Int64(std::stoll(row.at("created_at"))); // 创建时间
                } catch (const std::exception &e) { // 捕获异常
                    LOG_ERROR << "解析收款码数据失败: " << e.what(); // 记录错误
                    RESP_ERR(cb, "数据解析失败"); // 返回错误
                    return;
                }
                RESP_OK(cb, data); // 返回成功响应
            } else { // 如果没有结果
                Json::Value emptyData; // 空数据
                RESP_OK(cb, emptyData); // 返回空响应
            }
        } catch (const std::exception &e) { // 捕获异常
            RESP_ERR(cb, std::string("数据库错误: ") + e.what()); // 返回错误
        }
    }

    // 代理访问本地文件方法
    void proxyFile(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) // 响应回调函数
    {
        auto filename = req->getParameter("file"); // 获取文件名参数
        if (filename.empty()) { RESP_ERR(cb, "file 参数不能为空"); return; } // 文件名为空

        try {
            std::string filepath = "./uploads/" + filename; // 构建文件路径
            std::ifstream file(filepath, std::ios::binary); // 以二进制模式打开文件
            if (!file.is_open()) { RESP_ERR(cb, "文件不存在"); return; } // 文件不存在

            std::stringstream buffer; // 创建字符串流缓冲区
            buffer << file.rdbuf(); // 读取文件内容
            std::string fileData = buffer.str(); // 获取文件数据

            auto resp = drogon::HttpResponse::newHttpResponse(); // 创建 HTTP 响应
            resp->setStatusCode(drogon::HttpStatusCode::k200OK); // 设置状态码为 200
            resp->setContentTypeCode(drogon::CT_IMAGE_JPG); // 设置内容类型为图片
            resp->setBody(fileData); // 设置响应体
            cb(resp); // 回调响应
        } catch (const std::exception &e) { // 捕获异常
            RESP_ERR(cb, std::string("获取文件失败: ") + e.what()); // 返回错误
        }
    }

private:
    // 获取文件扩展名方法
    static std::string getExt(const std::string &filename) { // 文件名
        size_t pos = filename.rfind('.'); // 查找最后一个点
        return pos == std::string::npos ? "" : filename.substr(pos); // 返回扩展名
    }

    // 生成随机字符串方法
    static std::string randStr(size_t len) { // 字符串长度
        static std::mt19937 rng((unsigned)std::random_device{}()); // 随机数生成器
        static const char *chars = "0123456789abcdefghijklmnopqrstuvwxyz"; // 字符集
        std::string result; // 结果字符串
        std::uniform_int_distribution<size_t> dist(0, 35); // 均匀分布
        for (size_t i = 0; i < len; ++i) { // 循环生成字符
            result += chars[dist(rng)]; // 添加随机字符
        }
        return result; // 返回随机字符串
    }
};
