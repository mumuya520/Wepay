#pragma once

#include <drogon/drogon.h>
#include <json/json.h>
#include <string>
#include <algorithm>

class UpayProxy {
private:
    static std::string rewriteAbsolutePaths(const std::string &body, const std::string &prefix) {
        std::string out = body;
        const std::pair<const char *, const char *> replacements[] = {
            {"hx-post=\"/", "hx-post=\""},
            {"hx-get=\"/", "hx-get=\""},
            {"hx-delete=\"/", "hx-delete=\""},
            {"hx-put=\"/", "hx-put=\""},
            {"hx-patch=\"/", "hx-patch=\""},
            {"href=\"/", "href=\""},
            {"src=\"/", "src=\""},
            {"fetch(\"/", "fetch(\""},
            {"fetch('/", "fetch('"},
            {"window.location.href = \"/", "window.location.href = \""},
            {"window.location.href = '/", "window.location.href = '"},
            {"location.href = \"/", "location.href = \""},
            {"location.href = '/", "location.href = '"},
            {"url: \"/", "url: \""},
            {"url: '/", "url: '"},
        };

        for (const auto &[from, to] : replacements) {
            std::string needle = from;
            std::string replacement = std::string(to) + prefix + "/";
            size_t pos = 0;
            while ((pos = out.find(needle, pos)) != std::string::npos) {
                out.replace(pos, needle.size(), replacement);
                pos += replacement.size();
            }
        }

        return out;
    }

public:
    static void setup(const Json::Value &cfg) {
        if (!cfg.isMember("upay_shared"))
            return;

        const auto &c = cfg["upay_shared"];
        if (!c.get("enabled", false).asBool())
            return;

        std::string prefix = c.get("prefix", "/upay").asString();
        std::string backend = c.get("base_url", "http://127.0.0.1:8090").asString();
        while (!prefix.empty() && prefix.back() == '/')
            prefix.pop_back();
        if (prefix.empty())
            prefix = "/upay";

        const std::string pfx = prefix;
        const std::string backendUrl = backend;

        drogon::app().registerPreRoutingAdvice(
            [pfx, backendUrl](const drogon::HttpRequestPtr &req,
                              drogon::AdviceCallback &&cb,
                              drogon::AdviceChainCallback &&ccb) {
                const std::string &path = req->path();
                bool match = (path == pfx) ||
                             (path.size() > pfx.size() && path[pfx.size()] == '/' &&
                              path.rfind(pfx, 0) == 0);
                if (!match) {
                    ccb();
                    return;
                }

                if (path == pfx) {
                    auto r = drogon::HttpResponse::newRedirectionResponse(pfx + "/");
                    cb(r);
                    return;
                }

                std::string subPath = path.substr(pfx.size());
                if (!subPath.empty() && subPath.front() == '/')
                    subPath.erase(0, 1);

                std::string qs = req->query();
                if (!qs.empty())
                    subPath += "?" + qs;

                auto fwdReq = drogon::HttpRequest::newHttpRequest();
                fwdReq->setMethod(req->method());
                fwdReq->setPath("/" + subPath);

                for (auto &[k, v] : req->headers()) {
                    std::string lk = k;
                    std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
                    if (lk == "host" || lk == "content-length")
                        continue;
                    fwdReq->addHeader(k, v);
                }

                fwdReq->addHeader("X-Forwarded-Prefix", pfx);
                fwdReq->addHeader("X-Forwarded-Proto", req->peerCertificate() ? "https" : "http");
                fwdReq->addHeader("X-Forwarded-Host", req->getHeader("host"));

                std::string body(req->body());
                if (!body.empty()) {
                    fwdReq->setBody(std::move(body));
                    std::string ct = req->getHeader("content-type");
                    if (!ct.empty())
                        fwdReq->addHeader("Content-Type", ct);
                }

                auto client = drogon::HttpClient::newHttpClient(backendUrl);
                client->sendRequest(
                    fwdReq,
                    [cb = std::move(cb), pfx](drogon::ReqResult result,
                                              const drogon::HttpResponsePtr &resp) mutable {
                        if (result == drogon::ReqResult::Ok && resp) {
                            auto r = drogon::HttpResponse::newHttpResponse();
                            r->setStatusCode(resp->statusCode());
                            std::string contentType;
                            for (auto &kv : resp->headers()) {
                                std::string lk = kv.first;
                                std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
                                if (lk == "content-length" || lk == "transfer-encoding" ||
                                    lk == "connection" || lk == "keep-alive" ||
                                    lk == "proxy-authenticate" || lk == "proxy-authorization" ||
                                    lk == "te" || lk == "trailers" || lk == "upgrade")
                                    continue;
                                if (lk == "content-type") {
                                    contentType = kv.second;
                                }
                                if (lk == "location") {
                                    std::string location = kv.second;
                                    if (!location.empty() && location.front() == '/' &&
                                        location.rfind(pfx + "/", 0) != 0 && location != pfx) {
                                        location = pfx + location;
                                    }
                                    r->addHeader(kv.first, location);
                                    continue;
                                }
                                r->addHeader(kv.first, kv.second);
                            }
                            auto setCookie = resp->getHeader("set-cookie");
                            if (!setCookie.empty())
                                r->addHeader("Set-Cookie", setCookie);

                            std::string responseBody{resp->body()};
                            if (contentType.find("text/html") != std::string::npos ||
                                contentType.find("javascript") != std::string::npos) {
                                responseBody = rewriteAbsolutePaths(responseBody, pfx);
                            }

                            r->setBody(std::move(responseBody));
                            if (!contentType.empty())
                                r->setContentTypeString(contentType);
                            cb(r);
                        } else {
                            auto r = drogon::HttpResponse::newHttpResponse();
                            r->setStatusCode(drogon::k502BadGateway);
                            r->setContentTypeCode(drogon::CT_TEXT_PLAIN);
                            r->setBody("UPay service unavailable (8090 not reachable)");
                            cb(r);
                        }
                    });
            });

        LOG_INFO << "[UpayProxy] reverse proxy registered: " << pfx << "/* -> " << backendUrl;
    }
};
