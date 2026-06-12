// WePay-Cpp — Swagger/OpenAPI 文档服务
// GET /swagger            Swagger UI 页面(CDN 版，无需打包前端)
// GET /swagger/openapi.json   OpenAPI 3.0 规范 JSON
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include "AjaxResult.h" // AJAX 响应结果

class SwaggerCtrl : public drogon::HttpController<SwaggerCtrl> {
public:
    METHOD_LIST_BEGIN
        ADD_METHOD_TO(SwaggerCtrl::ui,   "/swagger",              drogon::Get);
        ADD_METHOD_TO(SwaggerCtrl::spec, "/swagger/openapi.json", drogon::Get);
    METHOD_LIST_END

    void ui(const drogon::HttpRequestPtr &,
            std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        static const char *html = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="UTF-8"/>
  <title>WePay API 文档</title>
  <link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/swagger-ui-dist@5.17.14/swagger-ui.css"/>
  <style>body{margin:0;font-family:sans-serif}#swagger-ui{max-width:1400px;margin:0 auto}</style>
</head>
<body>
  <div id="swagger-ui"></div>
  <script src="https://cdn.jsdelivr.net/npm/swagger-ui-dist@5.17.14/swagger-ui-bundle.js"></script>
  <script src="https://cdn.jsdelivr.net/npm/swagger-ui-dist@5.17.14/swagger-ui-standalone-preset.js"></script>
  <script>
    window.onload = () => {
      SwaggerUIBundle({
        url: '/swagger/openapi.json',
        dom_id: '#swagger-ui',
        deepLinking: true,
        presets: [SwaggerUIBundle.presets.apis, SwaggerUIStandalonePreset],
        layout: 'StandaloneLayout'
      });
    };
  </script>
</body>
</html>)HTML";
        auto r = drogon::HttpResponse::newHttpResponse();
        r->setContentTypeCode(drogon::CT_TEXT_HTML);
        r->setBody(html);
        cb(r);
    }

    void spec(const drogon::HttpRequestPtr &,
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) {
        Json::Value spec;
        spec["openapi"] = "3.0.3";
        spec["info"]["title"] = "WePay-Cpp 第三方支付系统 API";
        spec["info"]["version"] = "2.0.0";
        spec["info"]["description"] =
            "多商户、多支付通道、统一支付网关、异步通知、设备管理的支付平台 API。\n\n"
            "路由前缀:\n"
            "- `/admin/api/` 管理员后台(需 JWT)\n"
            "- `/merchant/api/` 商户后台(需 JWT)\n"
            "- `/gateway/` 统一支付网关(签名)\n"
            "- `/notify/` 第三方异步回调\n"
            "- `/device/` 设备上报\n";

        Json::Value srv; srv["url"] = "/"; srv["description"] = "当前服务器";
        spec["servers"].append(srv);

        // 安全方案
        spec["components"]["securitySchemes"]["bearerAuth"]["type"] = "http";
        spec["components"]["securitySchemes"]["bearerAuth"]["scheme"] = "bearer";
        spec["components"]["securitySchemes"]["bearerAuth"]["bearerFormat"] = "JWT";

        // 通用响应 schema
        auto &schemas = spec["components"]["schemas"];
        schemas["AjaxResult"]["type"] = "object";
        schemas["AjaxResult"]["properties"]["code"] = jobj("integer", "1=成功 0=失败");
        schemas["AjaxResult"]["properties"]["msg"]  = jobj("string", "消息");
        schemas["AjaxResult"]["properties"]["data"] = jobj("object", "数据");

        // 路径定义
        auto &paths = spec["paths"];
        addAuth(paths);
        addMerchant(paths);
        addChannel(paths);
        addOrder(paths);
        addSettle(paths);
        addGateway(paths);
        addDevice(paths);

        Json::StreamWriterBuilder wb;
        wb["indentation"] = "  ";
        std::string json = Json::writeString(wb, spec);
        auto r = drogon::HttpResponse::newHttpResponse();
        r->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        r->setBody(json);
        cb(r);
    }

private:
    static Json::Value jobj(const std::string &type, const std::string &desc) {
        Json::Value j; j["type"] = type; j["description"] = desc; return j;
    }
    static Json::Value bodyRef(const std::string &example = "{}") {
        Json::Value j;
        j["required"] = true;
        j["content"]["application/json"]["schema"]["type"] = "object";
        if (!example.empty())
            j["content"]["application/json"]["example"] = example;
        return j;
    }
    static Json::Value okResp() {
        Json::Value j;
        j["200"]["description"] = "OK";
        j["200"]["content"]["application/json"]["schema"]["$ref"] = "#/components/schemas/AjaxResult";
        return j;
    }
    static Json::Value secArr() {
        Json::Value arr(Json::arrayValue);
        Json::Value o; o["bearerAuth"] = Json::Value(Json::arrayValue);
        arr.append(o);
        return arr;
    }

    static void addAuth(Json::Value &paths) {
        auto &login = paths["/admin/api/auth/login"]["post"];
        login["tags"].append("认证"); login["summary"] = "管理员登录";
        login["requestBody"] = bodyRef(R"({"username":"admin","password":"admin"})");
        login["responses"] = okResp();

        auto &refresh = paths["/admin/api/auth/refresh"]["post"];
        refresh["tags"].append("认证"); refresh["summary"] = "刷新 access token";
        refresh["requestBody"] = bodyRef(R"({"refresh_token":"xxx"})");
        refresh["responses"] = okResp();

        auto &logout = paths["/admin/api/auth/logout"]["post"];
        logout["tags"].append("认证"); logout["summary"] = "登出";
        logout["security"] = secArr(); logout["responses"] = okResp();

        auto &mchLogin = paths["/merchant/api/auth/login"]["post"];
        mchLogin["tags"].append("认证"); mchLogin["summary"] = "商户登录";
        mchLogin["requestBody"] = bodyRef(R"({"username":"mch001","password":"xxx"})");
        mchLogin["responses"] = okResp();
    }

    static void addMerchant(Json::Value &paths) {
        auto &list = paths["/admin/api/merchant/list"]["get"];
        list["tags"].append("商户管理"); list["summary"] = "商户列表";
        list["security"] = secArr();
        Json::Value q1; q1["name"] = "page"; q1["in"] = "query"; q1["schema"]["type"] = "integer";
        Json::Value q2; q2["name"] = "size"; q2["in"] = "query"; q2["schema"]["type"] = "integer";
        Json::Value q3; q3["name"] = "search"; q3["in"] = "query"; q3["schema"]["type"] = "string";
        list["parameters"].append(q1); list["parameters"].append(q2); list["parameters"].append(q3);
        list["responses"] = okResp();

        auto &add = paths["/admin/api/merchant/add"]["post"];
        add["tags"].append("商户管理"); add["summary"] = "新增商户";
        add["security"] = secArr();
        add["requestBody"] = bodyRef(R"({"username":"mch001","password":"xxx","mch_name":"测试商户","rate":"1.00"})");
        add["responses"] = okResp();

        auto &edit = paths["/admin/api/merchant/edit"]["post"];
        edit["tags"].append("商户管理"); edit["summary"] = "编辑商户";
        edit["security"] = secArr();
        edit["requestBody"] = bodyRef(R"({"id":1,"mch_name":"新名","rate":"0.8"})");
        edit["responses"] = okResp();
    }

    static void addChannel(Json::Value &paths) {
        auto &list = paths["/admin/api/channel/list"]["get"];
        list["tags"].append("通道管理"); list["summary"] = "通道列表";
        list["security"] = secArr(); list["responses"] = okResp();

        auto &add = paths["/admin/api/channel/add"]["post"];
        add["tags"].append("通道管理"); add["summary"] = "新增通道";
        add["security"] = secArr();
        add["requestBody"] = bodyRef(R"({"channel_code":"wx_native","channel_name":"微信Native",
"pay_type":"wxpay","plugin":"wxpay_native","rate":"0.6",
"params_json":"{\"appid\":\"...\",\"mchid\":\"...\"}"})");
        add["responses"] = okResp();
    }

    static void addOrder(Json::Value &paths) {
        auto &list = paths["/admin/api/order/list"]["get"];
        list["tags"].append("订单管理"); list["summary"] = "订单列表";
        list["security"] = secArr(); list["responses"] = okResp();

        auto &close = paths["/admin/api/order/close/{id}"]["post"];
        close["tags"].append("订单管理"); close["summary"] = "关闭订单";
        close["security"] = secArr();
        Json::Value p; p["name"] = "id"; p["in"] = "path"; p["required"] = true;
        p["schema"]["type"] = "string";
        close["parameters"].append(p); close["responses"] = okResp();
    }

    static void addSettle(Json::Value &paths) {
        auto &list = paths["/admin/api/settle/list"]["get"];
        list["tags"].append("结算管理"); list["summary"] = "结算列表";
        list["security"] = secArr(); list["responses"] = okResp();

        auto &apply = paths["/merchant/api/settle/apply"]["post"];
        apply["tags"].append("结算管理"); apply["summary"] = "商户申请结算";
        apply["security"] = secArr();
        apply["requestBody"] = bodyRef(R"({"amount":"100.00"})");
        apply["responses"] = okResp();
    }

    static void addGateway(Json::Value &paths) {
        auto &create = paths["/gateway/create"]["post"];
        create["tags"].append("支付网关"); create["summary"] = "统一下单";
        create["description"] = "商户通过签名调用此接口下单，返回支付链接/二维码。需传 mch_id/pay_type/amount/out_trade_no/sign。";
        create["requestBody"] = bodyRef(R"({"mch_id":"M100001","out_trade_no":"T20260503001",
"pay_type":"wxpay","amount":"10.00","subject":"测试商品","notify_url":"https://...","sign":"xxx"})");
        create["responses"] = okResp();

        auto &query = paths["/gateway/query"]["post"];
        query["tags"].append("支付网关"); query["summary"] = "查询订单";
        query["responses"] = okResp();

        auto &refund = paths["/gateway/refund"]["post"];
        refund["tags"].append("支付网关"); refund["summary"] = "申请退款";
        refund["responses"] = okResp();
    }

    static void addDevice(Json::Value &paths) {
        auto &heart = paths["/device/heart"]["post"];
        heart["tags"].append("设备"); heart["summary"] = "设备心跳";
        heart["requestBody"] = bodyRef(R"({"device_no":"D001","t":1700000000,"sign":"xxx"})");
        heart["responses"] = okResp();

        auto &push = paths["/device/push"]["post"];
        push["tags"].append("设备"); push["summary"] = "设备推送收款";
        push["responses"] = okResp();
    }
};
