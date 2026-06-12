// WePay-Cpp — 嵌入式前端托管
// 编译时将前端打包文件嵌入 exe，运行时从内存提供服务
// 构建方式：cmake -DEMBED_FRONTEND=ON -DEMBED_FRONTEND_DIR=UI/dist ..
//
// config.json:
//   "embedded_frontend": {
//       "enabled": true,
//       "spa_mode": true,
//       "api_prefix": "/prod-api"
//   }
//
// 与 "frontend" 配置互斥，两者不能同时启用
#pragma once

#ifdef WEPAY_EMBEDDED_FRONTEND

#include <drogon/drogon.h>
#include <trantor/utils/Logger.h>
#include <string>
#include <unordered_map>

struct EmbeddedResource {
    const unsigned char* data;
    size_t size;
    const char* contentType;
};

// 由 cmake/EmbedResources.cmake 生成的 EmbeddedResources.cc 提供
extern const std::unordered_map<std::string, EmbeddedResource>& getEmbeddedResources();
extern size_t getEmbeddedResourceCount();

class EmbeddedFrontend {
public:
    // 注册嵌入式前端文件服务
    // apiPrefix: API 路径前缀（如 /prod-api），这些路径不做静态文件匹配
    // spaMode:   true 时非 API 路径回退到 index.html
    static void registerHandlers(const std::string& apiPrefix = "/prod-api",
                                  bool spaMode = true) {
        auto& files = getEmbeddedResources();
        LOG_INFO << "[EmbeddedFrontend] 已嵌入 " << files.size()
                 << " 个前端文件 | SPA=" << spaMode << " | API=" << apiPrefix;

        drogon::app().registerPreRoutingAdvice(
            [&files, apiPrefix, spaMode](
                const drogon::HttpRequestPtr& req,
                drogon::AdviceCallback&& cb,
                drogon::AdviceChainCallback&& ccb) {

                std::string path = req->path();

                // API 前缀路径 → 剥掉前缀，交给后端控制器
                if (!apiPrefix.empty() && apiPrefix != "/" &&
                    path.rfind(apiPrefix, 0) == 0) {
                    std::string newPath = path.substr(apiPrefix.size());
                    if (newPath.empty()) newPath = "/";
                    req->setPath(newPath);
                    ccb();
                    return;
                }

                // 精确匹配嵌入文件
                auto it = files.find(path);
                if (it != files.end()) {
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setBody(std::string(
                        reinterpret_cast<const char*>(it->second.data),
                        it->second.size));
                    resp->addHeader("Content-Type", it->second.contentType);
                    // index.html 不缓存，其他资源长缓存（带 hash 文件名）
                    if (path == "/" || path == "/index.html") {
                        resp->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
                    } else {
                        resp->addHeader("Cache-Control", "public, max-age=31536000, immutable");
                    }
                    cb(resp);
                    return;
                }

                // SPA 回退：非文件路径（无扩展名）→ 返回 index.html
                if (spaMode && path.find('.') == std::string::npos) {
                    auto idx = files.find("/index.html");
                    if (idx != files.end()) {
                        auto resp = drogon::HttpResponse::newHttpResponse();
                        resp->setBody(std::string(
                            reinterpret_cast<const char*>(idx->second.data),
                            idx->second.size));
                        resp->addHeader("Content-Type", "text/html; charset=utf-8");
                        resp->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
                        cb(resp);
                        return;
                    }
                }

                // 非前端路径，交给 drogon 后续处理
                ccb();
            });
    }
};

#endif // WEPAY_EMBEDDED_FRONTEND
