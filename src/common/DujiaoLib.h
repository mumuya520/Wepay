// WePay-Cpp — Dujiao 库加载器
// DujiaoLib.h — 加载 dujiao.dll 并封装 C 函数
// 提供进程级单例：负责 DLL 句柄管理 + token 缓存 + 函数符号解析 + FFI HTTP 直派
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <mutex> // 互斥锁
#include <unordered_map> // 哈希表
#include <chrono> // 时间库
#include <atomic> // 原子操作
#include <map>
#include <drogon/drogon.h>
#include <json/json.h>

#ifdef _WIN32
#  include <windows.h>
#  define DJ_LIB_HANDLE HMODULE
#  define DJ_LOAD_LIB(p) LoadLibraryA(p)
#  define DJ_GET_PROC GetProcAddress
#  define DJ_FREE_LIB FreeLibrary
#else
#  include <dlfcn.h>
#  include <unistd.h>
#  define DJ_LIB_HANDLE void*
#  define DJ_LOAD_LIB(p) dlopen(p, RTLD_LAZY|RTLD_GLOBAL)
#  define DJ_GET_PROC dlsym
#  define DJ_FREE_LIB dlclose
#endif

// FFI HTTP 响应结构体
// 用于 FFI 模式下 dujiao.dll 返回的 HTTP 响应
struct DujiaoHTTPResponse {
    // HTTP 状态码（默认 500）
    int status{500};
    // 响应头
    std::map<std::string, std::string> headers;
    // 响应体
    std::string body;
    // 是否成功
    bool ok{false};
    // 错误消息
    std::string err;
};

// Dujiao 库加载器类
// 职责：
//   1. 动态加载 dujiao.dll（Windows）或 libdujiao.so（Unix）
//   2. 解析导出的 C 函数符号
//   3. 提供 FFI 直派 HTTP 请求接口
//   4. 缓存 SSO token（带自动过期刷新）
//   5. 管理 DLL 生命周期
class DujiaoLib {
public:
    // 获取单例实例
    static DujiaoLib &instance() {
        // 静态单例
        static DujiaoLib inst;
        return inst;
    }

    // 加载 DLL 并解析所有导出函数
    // 参数 dllPath：DLL 文件路径
    // 返回：是否加载成功
    // 说明：
    //   1. 幂等操作（多次调用不会重复加载）
    //   2. 解析所有必需的导出函数
    //   3. 记录缺失的可选函数（SSO、FFI）
    bool load(const std::string &dllPath) {
        // 加锁保护并发访问
        std::lock_guard<std::mutex> lk(mtx_);
        // 如果已加载，直接返回
        if (handle_)
            return true;
        // 加载 DLL
        handle_ = DJ_LOAD_LIB(dllPath.c_str());
        // 检查加载是否成功
        if (!handle_) {
            // 记录错误日志
            LOG_ERROR << "[DujiaoLib] load failed: " << dllPath
#ifdef _WIN32
                      << " err=" << GetLastError();
#else
                      << " err=" << dlerror();
#endif
            return false;
        }

        // ── 解析所有导出函数 ──────────────────────────────
        // 解析 DJ_Init（初始化）
        DJ_Init              = (Fn_DJ_Init)             DJ_GET_PROC(handle_, "DJ_Init");
        // 解析 DJ_StartServer（启动 HTTP 服务）
        DJ_StartServer       = (Fn_DJ_StartServer)      DJ_GET_PROC(handle_, "DJ_StartServer");
        // 解析 DJ_StopServer（停止 HTTP 服务）
        DJ_StopServer        = (Fn_DJ_StopServer)       DJ_GET_PROC(handle_, "DJ_StopServer");
        // 解析 DJ_IssueAccessToken（签发 SSO token，可选）
        DJ_IssueAccessToken  = (Fn_DJ_IssueAccessToken) DJ_GET_PROC(handle_, "DJ_IssueAccessToken");
        // 解析 DJ_BuildEngine（构建 Gin engine，可选）
        DJ_BuildEngine       = (Fn_DJ_BuildEngine)      DJ_GET_PROC(handle_, "DJ_BuildEngine");
        // 解析 DJ_HandleHTTPRequest（FFI 直派，可选）
        DJ_HandleHTTPRequest = (Fn_DJ_HandleHTTPRequest)DJ_GET_PROC(handle_, "DJ_HandleHTTPRequest");
        // 解析 DJ_FreeString（释放字符串）
        DJ_FreeString        = (Fn_DJ_FreeString)       DJ_GET_PROC(handle_, "DJ_FreeString");
        // 解析 DJ_LastError（获取错误消息）
        DJ_LastError         = (Fn_DJ_LastError)        DJ_GET_PROC(handle_, "DJ_LastError");

        // ── 检查必需的导出函数 ──────────────────────────────
        // 检查必需的函数是否存在
        if (!DJ_Init || !DJ_StartServer || !DJ_StopServer ||
            !DJ_FreeString || !DJ_LastError) {
            // 记录错误日志
            LOG_ERROR << "[DujiaoLib] missing exports in " << dllPath;
            // 卸载 DLL
            DJ_FREE_LIB(handle_);
            // 清空句柄
            handle_ = nullptr;
            return false;
        }

        // ── 检查可选的导出函数 ──────────────────────────────
        // 检查 SSO token 函数
        if (!DJ_IssueAccessToken) {
            LOG_WARN << "[DujiaoLib] DJ_IssueAccessToken not exported — SSO disabled";
        }
        // 检查 FFI 函数
        if (!DJ_BuildEngine || !DJ_HandleHTTPRequest) {
            LOG_WARN << "[DujiaoLib] DJ_BuildEngine/DJ_HandleHTTPRequest not exported "
                        "— FFI 模式不可用，将退化到 loopback HTTP";
        }

        // 标记已加载
        loaded_ = true;
        // 记录加载成功日志
        LOG_INFO << "[DujiaoLib] loaded " << dllPath;
        return true;
    }

    // 检查 FFI 模式是否可用
    // 返回：是否支持 FFI 直派
    bool hasFFI() const {
        return DJ_BuildEngine != nullptr && DJ_HandleHTTPRequest != nullptr;
    }

    // 构建 Gin engine（FFI 模式）
    // 返回：返回码（0 表示成功）
    int buildEngine() {
        // 检查是否已加载且函数存在
        if (!loaded_ || !DJ_BuildEngine)
            return -100;
        // 调用 DLL 函数
        return DJ_BuildEngine();
    }

    // ── FFI 直派 HTTP 请求 ──────────────────────────────
    // 参数 method：HTTP 方法（GET、POST 等）
    // 参数 path：请求路径
    // 参数 rawQuery：查询字符串
    // 参数 headers：请求头
    // 参数 body：请求体
    // 返回：HTTP 响应
    // 说明：
    //   1. 直接调用 dujiao.dll 的 Gin engine（不经过 TCP）
    //   2. 零拷贝高性能
    //   3. 返回值为 JSON 格式（包含 status、headers、body_b64）
    DujiaoHTTPResponse handleRequest(const std::string &method,
                                     const std::string &path,
                                     const std::string &rawQuery,
                                     const std::map<std::string, std::string> &headers,
                                     const std::string &body) {
        // 创建响应对象
        DujiaoHTTPResponse resp;
        // 检查是否已加载且函数存在
        if (!loaded_ || !DJ_HandleHTTPRequest) {
            resp.err = "FFI not available";
            return resp;
        }

        // ── 构建请求头 JSON ──────────────────────────────
        // 创建 JSON 对象
        Json::Value h(Json::objectValue);
        // 遍历所有请求头
        for (auto &kv : headers)
            h[kv.first] = kv.second;
        // 序列化为 JSON 字符串（无缩进）
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        std::string headersJson = Json::writeString(wb, h);

        // ── 调用 FFI 函数 ──────────────────────────────
        // 调用 dujiao.dll 的 HandleHTTPRequest 函数
        char *out = DJ_HandleHTTPRequest(
            const_cast<char*>(method.c_str()),
            const_cast<char*>(path.c_str()),
            const_cast<char*>(rawQuery.c_str()),
            const_cast<char*>(headersJson.c_str()),
            body.empty() ? nullptr : (void*)body.data(),
            (int)body.size());
        // 检查是否成功
        if (!out) {
            resp.err = lastError();
            return resp;
        }

        // 转换为字符串
        std::string outStr = out;
        // 释放 DLL 分配的内存
        DJ_FreeString(out);

        // ── 解析返回 JSON ──────────────────────────────
        // 创建 JSON 对象
        Json::Value j;
        // 解析 JSON 字符串
        if (!Json::Reader().parse(outStr, j)) {
            resp.err = "parse response json failed";
            return resp;
        }

        // 提取状态码
        resp.status = j.get("status", 500).asInt();
        // 提取响应头
        if (j.isMember("headers") && j["headers"].isObject()) {
            for (auto &k : j["headers"].getMemberNames()) {
                resp.headers[k] = j["headers"][k].asString();
            }
        }
        // 提取响应体（base64 编码）
        std::string b64 = j.get("body_b64", "").asString();
        // 如果有响应体，解码
        if (!b64.empty())
            resp.body = base64Decode(b64);
        // 标记成功
        resp.ok = true;
        return resp;
    }

    // 检查 DLL 是否已加载
    bool isLoaded() const {
        return loaded_.load();
    }

    // 初始化 Dujiao
    // 参数 configDir：配置目录
    // 返回：返回码（0 表示成功）
    // 说明：
    //   1. dujiao DJ_Init 会改变进程的工作目录
    //   2. 这会破坏 WePay 自己的相对路径（logs、web、agpay 等）
    //   3. 解决方案：保存 cwd → 调 DJ_Init → 立即恢复 cwd
    int init(const std::string &configDir) {
        // 检查是否已加载
        if (!loaded_)
            return -100;

        // ── 保存当前工作目录 ──────────────────────────────
#ifdef _WIN32
        // Windows 实现
        char oldCwd[MAX_PATH] = {};
        DWORD got = GetCurrentDirectoryA(MAX_PATH, oldCwd);
        // 调用 DJ_Init
        int rc = DJ_Init(const_cast<char*>(configDir.c_str()));
        // 恢复工作目录
        if (got > 0)
            SetCurrentDirectoryA(oldCwd);
#else
        // Unix 实现
        char oldCwd[4096] = {};
        // 保存当前工作目录
        if (getcwd(oldCwd, sizeof(oldCwd)) == nullptr)
            oldCwd[0] = 0;
        // 调用 DJ_Init
        int rc = DJ_Init(const_cast<char*>(configDir.c_str()));
        // 恢复工作目录
        if (oldCwd[0])
            chdir(oldCwd);
#endif
        return rc;
    }

    // 启动 HTTP 服务器
    // 参数 mode：运行模式（api 或 web，默认 api）
    // 返回：返回码（0 表示成功）
    int startServer(const std::string &mode = "api") {
        // 检查是否已加载
        if (!loaded_)
            return -100;
        // 调用 DLL 函数
        return DJ_StartServer(const_cast<char*>(mode.c_str()));
    }

    // 停止 HTTP 服务器
    void stopServer() {
        // 检查是否已加载
        if (!loaded_)
            return;
        // 调用 DLL 函数
        DJ_StopServer();
    }

    // 获取最后的错误消息
    // 返回：错误消息字符串
    std::string lastError() {
        // 检查是否已加载且函数存在
        if (!loaded_ || !DJ_LastError)
            return "";
        // 调用 DLL 函数
        char *p = DJ_LastError();
        // 转换为字符串
        std::string s = p ? p : "";
        // 释放 DLL 分配的内存
        if (p)
            DJ_FreeString(p);
        return s;
    }

    // ── SSO Token 缓存 ──────────────────────────────
    // 获取 username 对应的 dujiao 访问 token
    // 参数 username：用户名
    // 参数 expireHours：token 有效期（小时，0 表示使用 dujiao 默认值 24h）
    // 返回：token 字符串（失败返回空）
    // 说明：
    //   1. 支持 token 缓存（避免频繁调用 DLL）
    //   2. 缓存有效期比实际有效期短 1 小时（提前刷新）
    std::string issueToken(const std::string &username, int expireHours = 0) {
        // 检查是否已加载且函数存在
        if (!loaded_ || !DJ_IssueAccessToken)
            return "";

        // ── 检查缓存 ──────────────────────────────
        auto now = std::chrono::steady_clock::now();
        {
            // 加锁保护缓存访问
            std::lock_guard<std::mutex> lk(cacheMtx_);
            // 查找缓存
            auto it = tokenCache_.find(username);
            // 如果缓存存在且未过期
            if (it != tokenCache_.end() && it->second.expire > now) {
                return it->second.token;
            }
        }

        // ── 缓存未命中或已过期，调用 DLL 重签 ──────────────────────────────
        // 调用 DLL 函数签发 token
        char *p = DJ_IssueAccessToken(const_cast<char*>(username.c_str()),
                                       expireHours);
        // 检查是否成功
        if (!p) {
            // 获取错误消息
            std::string err = lastError();
            // 记录错误日志
            LOG_ERROR << "[DujiaoLib] DJ_IssueAccessToken('" << username << "') failed: " << err;
            return "";
        }

        // 转换为字符串
        std::string tok = p;
        // 释放 DLL 分配的内存
        DJ_FreeString(p);

        // ── 更新缓存 ──────────────────────────────
        // 计算有效期（提前 1 小时刷新）
        int hours = expireHours > 0 ? expireHours : 24;
        auto ttl = std::chrono::hours(std::max(1, hours - 1));
        // 加锁保护缓存更新
        std::lock_guard<std::mutex> lk(cacheMtx_);
        // 存储到缓存
        tokenCache_[username] = { tok, now + ttl };
        return tok;
    }

    // 强制清除 token 缓存
    // 参数 username：用户名
    // 说明：当 WePay 用户密码变更时调用，强制重新签发 token
    void invalidateToken(const std::string &username) {
        // 加锁保护缓存访问
        std::lock_guard<std::mutex> lk(cacheMtx_);
        // 删除缓存
        tokenCache_.erase(username);
    }

// 私有区域
private:
    // 私有构造函数（单例）
    DujiaoLib() = default;

    // 析构函数
    ~DujiaoLib() {
        // 如果 DLL 已加载
        if (handle_) {
            // 注意：不主动调用 stopServer 避免析构时序问题
            // 由 main 函数显式调用 stopServer
            // 卸载 DLL
            DJ_FREE_LIB(handle_);
        }
    }

    // 禁止拷贝构造
    DujiaoLib(const DujiaoLib&) = delete;
    // 禁止赋值操作
    DujiaoLib &operator=(const DujiaoLib&) = delete;

    // ── DLL 导出函数的函数指针类型定义 ──────────────────────────────
    // DJ_Init 函数指针类型（初始化）
    using Fn_DJ_Init              = int   (*)(char*);
    // DJ_StartServer 函数指针类型（启动服务）
    using Fn_DJ_StartServer       = int   (*)(char*);
    // DJ_StopServer 函数指针类型（停止服务）
    using Fn_DJ_StopServer        = void  (*)();
    // DJ_IssueAccessToken 函数指针类型（签发 token）
    using Fn_DJ_IssueAccessToken  = char* (*)(char*, int);
    // DJ_BuildEngine 函数指针类型（构建 engine）
    using Fn_DJ_BuildEngine       = int   (*)();
    // DJ_HandleHTTPRequest 函数指针类型（FFI 直派）
    using Fn_DJ_HandleHTTPRequest = char* (*)(char*, char*, char*, char*, void*, int);
    // DJ_FreeString 函数指针类型（释放字符串）
    using Fn_DJ_FreeString        = void  (*)(char*);
    // DJ_LastError 函数指针类型（获取错误消息）
    using Fn_DJ_LastError         = char* (*)();

    // ── DLL 句柄和加载状态 ──────────────────────────────
    // DLL 句柄（Windows 为 HMODULE，Unix 为 void*）
    DJ_LIB_HANDLE handle_{nullptr};
    // 是否已加载标志（原子操作）
    std::atomic<bool> loaded_{false};
    // 互斥锁（保护 DLL 加载）
    std::mutex mtx_;

    // ── DLL 导出函数指针 ──────────────────────────────
    // DJ_Init 函数指针
    Fn_DJ_Init              DJ_Init{nullptr};
    // DJ_StartServer 函数指针
    Fn_DJ_StartServer       DJ_StartServer{nullptr};
    // DJ_StopServer 函数指针
    Fn_DJ_StopServer        DJ_StopServer{nullptr};
    // DJ_IssueAccessToken 函数指针（可选）
    Fn_DJ_IssueAccessToken  DJ_IssueAccessToken{nullptr};
    // DJ_BuildEngine 函数指针（可选）
    Fn_DJ_BuildEngine       DJ_BuildEngine{nullptr};
    // DJ_HandleHTTPRequest 函数指针（可选）
    Fn_DJ_HandleHTTPRequest DJ_HandleHTTPRequest{nullptr};
    // DJ_FreeString 函数指针
    Fn_DJ_FreeString        DJ_FreeString{nullptr};
    // DJ_LastError 函数指针
    Fn_DJ_LastError         DJ_LastError{nullptr};

    // ── Base64 解码 ──────────────────────────────
    // 解码 base64 字符串
    // 参数 in：base64 编码的字符串
    // 返回：解码后的二进制数据
    // 说明：用于解码 FFI 返回的响应体（body_b64）
    static std::string base64Decode(const std::string &in) {
        // Base64 字符表
        static const std::string chars =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        // 输出缓冲区
        std::string out;
        // 累积值和位数
        int val = 0, valb = -8;
        // 遍历输入字符
        for (unsigned char c : in) {
            // 如果遇到填充符，停止
            if (c == '=')
                break;
            // 查找字符在字符表中的位置
            auto pos = chars.find((char)c);
            // 如果字符不在字符表中
            if (pos == std::string::npos) {
                // 跳过空白字符
                if (c == '\n' || c == '\r' || c == ' ')
                    continue;
                // 非法字符，返回已解码的部分
                return out;
            }
            // 累积值左移 6 位，加上新字符的值
            val = (val << 6) | (int)pos;
            // 累积位数加 6
            valb += 6;
            // 如果累积了至少 8 位
            if (valb >= 0) {
                // 提取高 8 位作为一个字节
                out.push_back((char)((val >> valb) & 0xFF));
                // 减少累积位数
                valb -= 8;
            }
        }
        // 返回解码结果
        return out;
    }

    // ── Token 缓存结构体 ──────────────────────────────
    // 缓存的 token 信息
    struct CachedToken {
        // Token 字符串
        std::string token;
        // Token 过期时间
        std::chrono::steady_clock::time_point expire;
    };

    // Token 缓存互斥锁
    std::mutex cacheMtx_;
    // Token 缓存（用户名 → token 信息）
    std::unordered_map<std::string, CachedToken> tokenCache_;
};
