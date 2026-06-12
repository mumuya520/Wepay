#pragma once

#include <drogon/HttpController.h>
#include "AlipayLib.h"

using namespace drogon;

class AlipayCtrl : public HttpController<AlipayCtrl> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AlipayCtrl::generateTransferScheme, "/api/alipay/transfer", Post);
    ADD_METHOD_TO(AlipayCtrl::generateAddFriendScheme, "/api/alipay/addfriend", Post);
    ADD_METHOD_TO(AlipayCtrl::generateRedPacketScheme, "/api/alipay/redpacket", Post);
    METHOD_LIST_END

    // 生成转账 Scheme
    void generateTransferScheme(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        auto json = req->getJsonObject();
        
        if (!json) {
            Json::Value resp;
            resp["code"] = -1;
            resp["msg"] = "请求体不是有效的 JSON";
            auto httpResp = HttpResponse::newHttpJsonResponse(resp);
            httpResp->setStatusCode(k400BadRequest);
            callback(httpResp);
            return;
        }
        
        std::string userID = (*json)["user_id"].asString();
        std::string amount = (*json)["amount"].asString();
        std::string memo = (*json)["memo"].asString();
        
        if (userID.empty() || amount.empty()) {
            Json::Value resp;
            resp["code"] = -1;
            resp["msg"] = "缺少必要参数: user_id 或 amount";
            auto httpResp = HttpResponse::newHttpJsonResponse(resp);
            httpResp->setStatusCode(k400BadRequest);
            callback(httpResp);
            return;
        }
        
        try {
            std::string scheme = AlipayLib::generateTransferScheme(userID, amount, memo);
            
            Json::Value resp;
            resp["code"] = 1;
            resp["msg"] = "生成成功";
            resp["scheme"] = scheme;
            auto httpResp = HttpResponse::newHttpJsonResponse(resp);
            httpResp->setStatusCode(k200OK);
            
            callback(httpResp);
        } catch (const std::exception& e) {
            Json::Value resp;
            resp["code"] = -1;
            resp["msg"] = std::string("生成失败: ") + e.what();
            auto httpResp = HttpResponse::newHttpJsonResponse(resp);
            httpResp->setStatusCode(k500InternalServerError);
            
            callback(httpResp);
        }
    }

    // 生成添加好友 Scheme
    void generateAddFriendScheme(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        auto json = req->getJsonObject();
        
        if (!json) {
            Json::Value resp;
            resp["code"] = -1;
            resp["msg"] = "请求体不是有效的 JSON";
            auto httpResp = HttpResponse::newHttpJsonResponse(resp);
            httpResp->setStatusCode(k400BadRequest);
            callback(httpResp);
            return;
        }
        
        std::string userID = (*json)["user_id"].asString();
        std::string loginID = (*json)["login_id"].asString();
        
        if (userID.empty() || loginID.empty()) {
            Json::Value resp;
            resp["code"] = -1;
            resp["msg"] = "缺少必要参数: user_id 或 login_id";
            auto httpResp = HttpResponse::newHttpJsonResponse(resp);
            httpResp->setStatusCode(k400BadRequest);
            callback(httpResp);
            return;
        }
        
        try {
            std::string scheme = AlipayLib::generateAddFriendScheme(userID, loginID);
            
            Json::Value resp;
            resp["code"] = 1;
            resp["msg"] = "生成成功";
            resp["scheme"] = scheme;
            auto httpResp = HttpResponse::newHttpJsonResponse(resp);
            httpResp->setStatusCode(k200OK);
            
            callback(httpResp);
        } catch (const std::exception& e) {
            Json::Value resp;
            resp["code"] = -1;
            resp["msg"] = std::string("生成失败: ") + e.what();
            auto httpResp = HttpResponse::newHttpJsonResponse(resp);
            httpResp->setStatusCode(k500InternalServerError);
            
            callback(httpResp);
        }
    }

    // 生成红包 Scheme
    void generateRedPacketScheme(const HttpRequestPtr& req, std::function<void(const HttpResponsePtr&)>&& callback) {
        auto json = req->getJsonObject();
        
        if (!json) {
            Json::Value resp;
            resp["code"] = -1;
            resp["msg"] = "请求体不是有效的 JSON";
            auto httpResp = HttpResponse::newHttpJsonResponse(resp);
            httpResp->setStatusCode(k400BadRequest);
            callback(httpResp);
            return;
        }
        
        std::string loginID = (*json)["login_id"].asString();
        std::string userID = (*json)["user_id"].asString();
        std::string userName = (*json)["user_name"].asString();
        std::string amount = (*json)["amount"].asString();
        std::string remark = (*json)["remark"].asString();
        
        if (loginID.empty() || userID.empty() || userName.empty() || amount.empty()) {
            Json::Value resp;
            resp["code"] = -1;
            resp["msg"] = "缺少必要参数: login_id, user_id, user_name 或 amount";
            auto httpResp = HttpResponse::newHttpJsonResponse(resp);
            httpResp->setStatusCode(k400BadRequest);
            callback(httpResp);
            return;
        }
        
        try {
            std::string scheme = AlipayLib::generateRedPacketScheme(loginID, userID, userName, amount, remark);
            
            Json::Value resp;
            resp["code"] = 1;
            resp["msg"] = "生成成功";
            resp["scheme"] = scheme;
            auto httpResp = HttpResponse::newHttpJsonResponse(resp);
            httpResp->setStatusCode(k200OK);
            
            callback(httpResp);
        } catch (const std::exception& e) {
            Json::Value resp;
            resp["code"] = -1;
            resp["msg"] = std::string("生成失败: ") + e.what();
            auto httpResp = HttpResponse::newHttpJsonResponse(resp);
            httpResp->setStatusCode(k500InternalServerError);
            
            callback(httpResp);
        }
    }
};
