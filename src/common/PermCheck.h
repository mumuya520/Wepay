// WePay-Cpp — 权限检查辅助
// 在 AdminAuthFilter 之后使用，根据请求头 X-Admin-Id / X-Admin-Super 判断权限
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <string> // 字符串库
#include "AjaxResult.h" // AJAX 响应结果
#include "RbacService.h" // RBAC 服务

// 权限检查宏 — 在控制器方法开头调用: REQUIRE_PERM(cb, "order:close");
// 此宏用于检查当前管理员是否拥有指定权限，如果没有权限则返回 403 错误
// 使用 do-while(0) 确保宏可以在任何地方使用（包括 if 语句中）
// 工作流程：
// 1. 从 HTTP 请求头获取超管标志（"X-Admin-Super"）和管理员 ID（"X-Admin-Id"）
// 2. 如果是超管（X-Admin-Super == "1"），则直接跳过权限检查
// 3. 调用 RBAC 服务检查管理员是否拥有指定权限
// 4. 如果没有权限，返回 403 错误响应，并退出当前函数
#define REQUIRE_PERM(cb, code) \
    do { \
        std::string _super = req->getHeader("X-Admin-Super"); \
        std::string _aidS  = req->getHeader("X-Admin-Id"); \
        if (_super == "1") break; \
        int _aid = 0; \
        try { _aid = std::stoi(_aidS); } catch(...) {} \
        if (!RbacService::hasPermission(_aid, code)) { \
            auto _r = drogon::HttpResponse::newHttpJsonResponse( \
                AjaxResult::error(403, std::string("无权限: ") + code)); \
            _r->setStatusCode(drogon::k403Forbidden); \
            cb(_r); return; \
        } \
    } while (0)
