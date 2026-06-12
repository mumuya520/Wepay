// WePay-Cpp — 设备密钥绑定状态检查接口
// GET /admin/api/auth/device-key-status    检查管理员绑定状态
// GET /merchant/api/auth/device-key-status 检查商户绑定状态
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/DeviceKeyEnforcement.h" // 设备密钥强制绑定
#include "../filters/AdminAuthFilter.h" // 管理员认证过滤器
#include "../filters/MerchantAuthFilter.h"

class DeviceKeyStatusCtrl : public drogon::HttpController<DeviceKeyStatusCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(DeviceKeyStatusCtrl::adminStatus,    "/admin/api/auth/device-key-status",    drogon::Get, "AdminAuthFilter");
        ADD_METHOD_TO(DeviceKeyStatusCtrl::merchantStatus, "/merchant/api/auth/device-key-status", drogon::Get, "MerchantAuthFilter");
    METHOD_LIST_END

    // 管理员检查绑定状态
    void adminStatus(const drogon::HttpRequestPtr &req,
                     std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string userIdStr = req->getHeader("X-User-Id");
        int userId = 0;
        try { userId = std::stoi(userIdStr); } catch (...) {}

        auto status = DeviceKeyEnforcement::getStatus(1, userId);

        Json::Value data;
        data["has_device_key"] = status.hasDeviceKey;
        data["device_count"] = status.deviceCount;
        data["is_required"] = status.isRequired;
        data["message"] = status.message;

        RESP_OK(cb, data);
    }

    // 商户检查绑定状态
    void merchantStatus(const drogon::HttpRequestPtr &req,
                        std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        std::string mchId = req->getHeader("X-Mch-Id");
        int mchIdInt = 0;
        try { mchIdInt = std::stoi(mchId); } catch (...) {}

        auto status = DeviceKeyEnforcement::getStatus(2, mchIdInt);

        Json::Value data;
        data["has_device_key"] = status.hasDeviceKey;
        data["device_count"] = status.deviceCount;
        data["is_required"] = status.isRequired;
        data["message"] = status.message;

        RESP_OK(cb, data);
    }
};
