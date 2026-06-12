// KoboldCppService.h — 本地 KoboldCpp 推理服务封装（header-only，基于 SyncHttp）
// KoboldCpp HTTP server 跑在 localhost:port，支持 OpenAI 兼容 + 原生 generate 接口
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <sstream> // 字符串流
#include <json/json.h> // JSON 库
#include "../common/SyncHttp.h" // 同步 HTTP 客户端
#include <trantor/utils/Logger.h> // 日志库

// KoboldCpp 服务类
class KoboldCppService {
public:
    // 获取单例
    static KoboldCppService& instance() {
        static KoboldCppService inst; // 静态单例
        return inst;
    }

    // 设置端口号
    void setPort(int port) { port_ = port; } // 设置 KoboldCpp 服务端口
    // 获取端口号
    int  port() const { return port_; } // 获取当前端口号

    // 探测 LLM 服务是否就绪（OpenAI 兼容：KoboldCpp / llama-server / vLLM 都支持）
    bool isReady() const {
        auto r = SyncHttp::get(
            "http://127.0.0.1:" + std::to_string(port_) + "/v1/models",
            {}, 2);
        return r.success && r.status == 200;
    }

    // OpenAI-compatible chat completions（适合对话场景）
    std::string chat(const std::string& message,
                     const std::string& systemPrompt = "",
                     float temperature = 0.7f, int maxTokens = 512) {
        Json::Value msgs(Json::arrayValue);
        if (!systemPrompt.empty()) {
            Json::Value sys; sys["role"] = "system"; sys["content"] = systemPrompt;
            msgs.append(sys);
        }
        Json::Value usr; usr["role"] = "user"; usr["content"] = message;
        msgs.append(usr);

        Json::Value req;
        req["model"]       = "koboldcpp";
        req["messages"]    = msgs;
        req["temperature"] = temperature;
        req["max_tokens"]  = maxTokens;
        Json::FastWriter fw;

        auto resp = SyncHttp::postJson(
            "http://127.0.0.1:" + std::to_string(port_) + "/v1/chat/completions",
            fw.write(req), {}, 30);

        if (!resp.success || resp.body.empty()) {
            lastError_ = "No response from KoboldCpp";
            return "";
        }
        Json::Value root; Json::Reader r;
        if (!r.parse(resp.body, root)) { lastError_ = "JSON parse error"; return resp.body; }
        try { return root["choices"][0]["message"]["content"].asString(); }
        catch (...) { return resp.body; }
    }

    // Raw text generation（适合 prompt 拼接场景）
    std::string generate(const std::string& prompt,
                         float temperature = 0.7f, int maxTokens = 512) {
        Json::Value req;
        req["prompt"]      = prompt;
        req["temperature"] = temperature;
        req["max_length"]  = maxTokens;
        Json::FastWriter fw;

        // 使用 OpenAI 兼容 /v1/completions，KoboldCpp / llama-server 都支持
        Json::Value oaiReq;
        oaiReq["prompt"]      = prompt;
        oaiReq["max_tokens"]  = maxTokens;
        oaiReq["temperature"] = temperature;
        auto resp = SyncHttp::postJson(
            "http://127.0.0.1:" + std::to_string(port_) + "/v1/completions",
            fw.write(oaiReq), {}, 30);

        if (!resp.success || resp.body.empty()) {
            lastError_ = "No response from KoboldCpp";
            return "";
        }
        Json::Value root; Json::Reader r;
        if (!r.parse(resp.body, root)) return resp.body;
        // OpenAI 兼容格式: choices[0].text
        if (root.isObject() && root["choices"].isArray() && !root["choices"].empty()) {
            const auto& c0 = root["choices"][0];
            if (c0.isObject()) return c0.get("text", "").asString();
        }
        // 兼容 KoboldCpp 旧格式
        if (root.isObject() && root["results"].isArray() && !root["results"].empty())
            return root["results"][0].get("text", "").asString();
        return resp.body;
    }

    // 调用 LLM 对可疑请求进行安全分析（返回风险评分 0-100）
    int analyzeRequest(const std::string& method, const std::string& path,
                       const std::string& query, const std::string& body) {
        if (!isReady()) return -1;  // LLM 不可用时跳过
        std::string prompt =
            "你是一个 Web 安全专家。分析以下 HTTP 请求，判断是否包含攻击行为。\n"
            "只回复一个 0-100 的整数数字，0=完全安全，100=高危攻击。\n"
            "HTTP 请求:\n"
            "Method: " + method + "\n"
            "Path: " + path + "\n"
            "Query: " + (query.size() > 200 ? query.substr(0,200) : query) + "\n"
            "Body: " + (body.size() > 300 ? body.substr(0,300) : body) + "\n"
            "风险评分(只输出数字):";
        std::string reply = generate(prompt, 0.1f, 8);
        try {
            // 从回复中提取第一个数字
            for (size_t i = 0; i < reply.size(); ++i) {
                if (std::isdigit(reply[i])) {
                    size_t end = i;
                    while (end < reply.size() && std::isdigit(reply[end])) ++end;
                    int score = std::stoi(reply.substr(i, end - i));
                    return std::min(100, std::max(0, score));
                }
            }
        } catch (...) {}
        return -1;
    }

    std::string lastError() const { return lastError_; }

private:
    KoboldCppService() = default;
    int         port_      = 5001;
    mutable std::string lastError_;
};
