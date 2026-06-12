// WePay-Cpp — 管理后台: 文件上传控制器
// 职责：通用文件上传功能（图片等）
//
// API 端点：
// POST /admin/api/upload/image  (multipart/form-data, 字段名 file)
// 返回 { "code":0, "data": { "url": "/uploads/xxxx.png" } }
//
// 文件保存到 ./uploads/ 目录，由 main.cc 注册静态路由提供访问
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <filesystem> // 文件系统库
#include <random> // 随机数库
#include <ctime> // C 时间库
#include <algorithm>
#include "../common/AjaxResult.h"
#include "../common/NotifyChannels.h"
#include "../filters/AdminAuthFilter.h"

class FileUploadCtrl : public drogon::HttpController<FileUploadCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(FileUploadCtrl::uploadImage, "/admin/api/upload/image",
                      drogon::Post, "AdminAuthFilter");
        ADD_METHOD_TO(FileUploadCtrl::deleteImage, "/admin/api/upload/image",
                      drogon::Delete, "AdminAuthFilter");
        ADD_METHOD_TO(FileUploadCtrl::serveFile, "/uploads/{1}",
                      drogon::Get);
    METHOD_LIST_END

    void uploadImage(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb)
    {
        drogon::MultiPartParser parser;
        if (parser.parse(req) != 0 || parser.getFiles().empty()) {
            RESP_ERR(cb, "请上传文件"); return;
        }

        auto &file = parser.getFiles()[0];
        std::string origName = file.getFileName();
        std::string ext = getExt(origName);

        // 只允许图片
        static const std::set<std::string> allowed = {
            ".jpg", ".jpeg", ".png", ".gif", ".webp", ".svg", ".bmp", ".ico"
        };
        std::string extLower = ext;
        std::transform(extLower.begin(), extLower.end(), extLower.begin(), ::tolower);
        if (allowed.find(extLower) == allowed.end()) {
            RESP_ERR(cb, "不支持的图片格式: " + ext); return;
        }

        // 限制大小 5MB
        if (file.fileLength() > 5 * 1024 * 1024) {
            RESP_ERR(cb, "文件大小不能超过 5MB"); return;
        }

        // 先落本地临时目录
        std::string uploadDir = "./uploads";
        std::filesystem::create_directories(uploadDir);
        std::string newName = std::to_string(std::time(nullptr)) + "_" + randStr(6) + extLower;
        std::string savePath = uploadDir + "/" + newName;
        file.saveAs(savePath);

        // 通过 OssService 上传（MinIO 不通自动回退本地）
        std::string keyName = "uploads/" + newName;
        std::string url = OssService::instance().upload(keyName, savePath);
        if (url.empty()) url = "/uploads/" + newName;  // 兜底

        Json::Value data;
        data["url"]      = url;
        data["filename"] = newName;
        data["size"]     = (Json::UInt64)file.fileLength();
        RESP_OK(cb, data);
    }

    void deleteImage(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb)
    {
        auto body = req->getJsonObject();
        if (!body) { RESP_ERR(cb, "格式错误"); return; }
        std::string url = (*body).get("url", "").asString();
        if (url.empty()) { RESP_ERR(cb, "url 必填"); return; }

        // 从 URL 提取文件名: /uploads/xxx.png → xxx.png
        std::string prefix = "/uploads/";
        std::string filename;
        if (url.rfind(prefix, 0) == 0) {
            filename = url.substr(prefix.size());
        } else {
            filename = url;
        }

        // 安全检查
        if (filename.empty() || filename.find("..") != std::string::npos ||
            filename.find('/') != std::string::npos ||
            filename.find('\\') != std::string::npos) {
            RESP_ERR(cb, "非法文件名"); return;
        }

        // 先尝试从 OSS 删除，再清理本地临时文件
        OssService::instance().remove("uploads/" + filename);
        std::string filePath = "./uploads/" + filename;
        if (std::filesystem::exists(filePath)) {
            std::filesystem::remove(filePath);
        }
        RESP_MSG(cb, "已删除");
    }

    void serveFile(const drogon::HttpRequestPtr &req,
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb,
                   const std::string &filename)
    {
        // 安全检查: 禁止路径穿越
        if (filename.find("..") != std::string::npos ||
            filename.find('/') != std::string::npos ||
            filename.find('\\') != std::string::npos) {
            cb(drogon::HttpResponse::newNotFoundResponse()); return;
        }

        std::string filePath = "./uploads/" + filename;
        if (!std::filesystem::exists(filePath)) {
            cb(drogon::HttpResponse::newNotFoundResponse()); return;
        }

        // 根据扩展名设置 Content-Type
        std::string ext = getExt(filename);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        static const std::map<std::string, drogon::ContentType> mimeMap = {
            {".jpg",  drogon::CT_IMAGE_JPG},
            {".jpeg", drogon::CT_IMAGE_JPG},
            {".png",  drogon::CT_IMAGE_PNG},
            {".gif",  drogon::CT_IMAGE_GIF},
            {".webp", drogon::CT_IMAGE_PNG},  // 近似
            {".svg",  drogon::CT_IMAGE_SVG_XML},
            {".bmp",  drogon::CT_IMAGE_PNG},  // 近似
            {".ico",  drogon::CT_IMAGE_PNG},  // 近似
        };

        auto resp = drogon::HttpResponse::newFileResponse(filePath);
        auto it = mimeMap.find(ext);
        if (it != mimeMap.end()) {
            resp->setContentTypeCode(it->second);
        }
        // 缓存 1 天
        resp->addHeader("Cache-Control", "public, max-age=86400");
        cb(resp);
    }

private:
    static std::string getExt(const std::string &name) {
        auto pos = name.rfind('.');
        if (pos == std::string::npos) return "";
        return name.substr(pos);
    }

    static std::string randStr(int len) {
        static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
        std::mt19937 rng((unsigned)std::random_device{}());
        std::uniform_int_distribution<int> dist(0, sizeof(chars) - 2);
        std::string s;
        s.reserve(len);
        for (int i = 0; i < len; ++i) s += chars[dist(rng)];
        return s;
    }
};
