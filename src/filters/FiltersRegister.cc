// WePay-Cpp — HttpMiddleware 手动注册
// 由于所有 Auth Filter 都设置了 isAutoCreation=false，
// 需要在主程序启动阶段显式调用 registerMiddleware。
// 本文件提供 registerWepayMiddlewares() 函数供 main.cc 调用。

#include <drogon/HttpAppFramework.h>
#include "AdminAuthFilter.h"
#include "MerchantAuthFilter.h"
#include "AgentAuthFilter.h"
#include "RateLimitFilter.h"

void registerWepayMiddlewares() {
    drogon::app().registerMiddleware(std::make_shared<AdminAuthFilter>());
    drogon::app().registerMiddleware(std::make_shared<MerchantAuthFilter>());
    drogon::app().registerMiddleware(std::make_shared<AgentAuthFilter>());
    drogon::app().registerMiddleware(std::make_shared<RateLimitFilter>());
}
