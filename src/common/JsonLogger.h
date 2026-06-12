// WePay-Cpp — JSON 格式日志工具
// 将 Trantor 的文本日志拦截并重格式化为 NDJSON（每行一个 JSON 对象）
// 支持日志轮转、文件大小限制、自动清理旧文件
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <fstream> // 文件流库
#include <mutex> // 互斥锁
#include <sstream> // 字符串流库
#include <chrono> // 时间库
#include <ctime> // C 时间库
#include <cstdio> // C 标准输入输出库
#include <cstring> // C 字符串库
#include <atomic> // 原子操作
#include <filesystem> // 文件系统库
#include <vector> // 向量容器
#include <algorithm> // 算法库
#include <trantor/utils/Logger.h> // Trantor 日志库
#include "TermColor.h" // 终端颜色库

/**
 * JsonLogger — 将 Trantor 的文本日志拦截并重格式化为 NDJSON
 *
 * 输出格式（每行一个 JSON 对象）：
 *   {"ts":"2026-03-28T03:41:23.123456","level":"INFO","thread":"0x1234",
 *    "file":"main.cc","line":150,"msg":"服务器已启动"}
 *
 * 用法（在 loadConfigFile 之后调用，保证覆盖 Drogon 的文件输出函数）：
 *   JsonLogger::instance().init("./logs", "wepay");
 */
// JSON 日志工具类，将 Trantor 的文本日志转换为 NDJSON 格式
class JsonLogger {
public:
    // 获取单例实例
    // 返回：JsonLogger 单例引用
    static JsonLogger& instance() {
        // 创建静态单例实例
        static JsonLogger inst;
        // 返回单例引用
        return inst;
    }

    // 初始化日志工具
    // 参数 logDir：日志文件保存目录
    // 参数 baseName：日志文件名前缀
    // 参数 maxFileSizeBytes：单个日志文件的大小上限（字节），超过后自动轮转；0 表示不限制
    // 参数 keepFiles：保留最近的日志文件数量（包含当前文件）
    void init(const std::string& logDir, const std::string& baseName,
              size_t maxFileSizeBytes = 100ULL * 1024 * 1024,
              int keepFiles = 5) {
        // 设置日志目录
        logDir_          = logDir;
        // 设置日志文件名前缀
        baseName_        = baseName;
        // 设置单文件大小上限
        maxFileSizeBytes_ = maxFileSizeBytes;
        // 设置保留文件数量
        keepFiles_       = keepFiles;
        // 初始化当前文件大小为 0
        currentSize_     = 0;
        // 创建日志目录（如果不存在）
        std::filesystem::create_directories(logDir_);
        // 打开日志文件
        openFile();

        // 覆盖 Trantor 的输出函数（channel -1 = 全局默认通道）
        rebind();
    }

    // 在 registerBeginningAdvice 中重新绑定（覆盖 drogon run() 内部的 setOutputFunction）
    // 这确保我们的日志输出函数优先级最高
    static void rebind() {
        // 设置 Trantor 显示本地时间
        trantor::Logger::setDisplayLocalTime(true);
        // 设置 Trantor 的输出函数为我们的 write 和 flush
        trantor::Logger::setOutputFunction(
            // Lambda 函数：接收日志消息和长度，调用 write() 方法
            [](const char* msg, uint64_t len) { instance().write(msg, len); },
            // Lambda 函数：调用 flush() 方法刷新缓冲区
            []() { instance().flush(); }
        );
    }

    // 启动完成后关闭控制台输出，日志只写文件
    // 参数 on：true 表示启用控制台输出，false 表示禁用
    void setConsole(bool on) {
        // 设置控制台输出标志
        console_ = on;
    }

    // ── 输出回调（由 Trantor 各线程调用）─
    // 接收日志消息并缓存
    // 参数 msg：日志消息指针
    // 参数 len：日志消息长度
    void write(const char* msg, uint64_t len) {
        // 获取互斥锁，保护共享数据
        std::lock_guard<std::mutex> lk(mu_);
        // 将消息追加到行缓冲区
        lineBuf_.append(msg, static_cast<size_t>(len));
        // 处理完整的日志行
        flushLines();
    }

    // 刷新日志缓冲区
    void flush() {
        // 获取互斥锁，保护共享数据
        std::lock_guard<std::mutex> lk(mu_);
        // 如果行缓冲区不为空
        if (!lineBuf_.empty()) {
            // 处理缓冲区中的日志行
            flushLines();
        }
        // 如果文件已打开
        if (file_.is_open())
            // 刷新文件缓冲区
            file_.flush();
    }

private:
    std::string   logDir_;
    std::string   baseName_;
    std::ofstream file_;
    std::mutex    mu_;
    std::string   lineBuf_;          // 未完成行的缓冲
    size_t        maxFileSizeBytes_ = 100ULL * 1024 * 1024;
    int           keepFiles_        = 5;
    std::atomic<size_t> currentSize_{0};
    std::string   currentPath_;

    // ── 文件管理 ──────────────────────────────────────────────────────────────
    // 打开新的日志文件
    void openFile() {
        // 获取当前时间戳
        std::time_t now = std::time(nullptr);
        // 将时间戳转换为本地时间结构
        std::tm* t = std::localtime(&now);
        // 时间戳字符串缓冲区
        char ts[32];
        // 格式化时间戳为 "YYMMDD-HHMMSS" 格式
        std::strftime(ts, sizeof(ts), "%y%m%d-%H%M%S", t);
        // 构建日志文件路径：目录 + 前缀 + 时间戳 + .jsonl 扩展名
        currentPath_ = logDir_ + "/" + baseName_ + "." + ts + ".jsonl";
        // 如果文件已打开，先关闭
        if (file_.is_open())
            // 关闭文件
            file_.close();
        // 以追加模式打开日志文件
        file_.open(currentPath_, std::ios::app);
        // 若追加到已有文件，记录当前大小
        // 检查文件是否存在
        currentSize_ = std::filesystem::exists(currentPath_)
            // 如果存在，获取文件大小
            ? (size_t)std::filesystem::file_size(currentPath_)
            // 如果不存在，大小为 0
            : 0;
    }

    // 检查是否需要轮转日志文件
    void rotateIfNeeded() {
        // 如果未设置大小限制，不轮转
        if (maxFileSizeBytes_ == 0)
            // 函数返回
            return;
        // 如果当前文件大小未超过限制，不轮转
        if (currentSize_ < maxFileSizeBytes_)
            // 函数返回
            return;
        // 打开新的日志文件
        openFile();
        // 清理旧的日志文件
        pruneOldFiles();
    }

    // 清理旧的日志文件，只保留最近的 N 个文件
    void pruneOldFiles() {
        // 如果未设置保留文件数，不清理
        if (keepFiles_ <= 0)
            // 函数返回
            return;
        // 存储日志文件路径的向量
        std::vector<std::filesystem::path> logs;
        // 错误代码对象
        std::error_code ec;
        // 遍历日志目录中的所有文件
        for (auto& e : std::filesystem::directory_iterator(logDir_, ec)) {
            // 如果不是普通文件，跳过
            if (!e.is_regular_file(ec))
                // 继续下一个文件
                continue;
            // 获取文件名
            auto name = e.path().filename().string();
            // 检查文件名是否符合日志文件命名规则
            if (name.rfind(baseName_ + ".", 0) == 0 && name.size() > 6
                && name.substr(name.size() - 6) == ".jsonl")
                // 将文件路径添加到列表
                logs.push_back(e.path());
        }
        // 按文件名（含时间戳）升序排序
        std::sort(logs.begin(), logs.end());
        // 删除超出保留数量的旧文件
        while ((int)logs.size() > keepFiles_) {
            // 删除最旧的文件（列表中的第一个）
            std::filesystem::remove(logs.front(), ec);
            // 从列表中移除已删除的文件
            logs.erase(logs.begin());
        }
    }

    // 按换行符切分、逐行处理日志缓冲区
    void flushLines() {
        // 查找换行符位置
        size_t pos;
        // 循环处理所有完整的日志行
        while ((pos = lineBuf_.find('\n')) != std::string::npos) {
            // 提取从开始到换行符的子字符串（一行日志）
            std::string line = lineBuf_.substr(0, pos);
            // 从缓冲区中删除已处理的行（包括换行符）
            lineBuf_.erase(0, pos + 1);
            // 如果行不为空
            if (!line.empty()) {
                // 将文本日志行转换为 JSON 格式
                std::string json = toJson(line);
                // 写入 JSON 文件（并检查是否需要轮转）
                if (file_.is_open()) {
                    // 将 JSON 写入文件
                    file_ << json << '\n';
                    // 刷新文件缓冲区
                    file_.flush();
                    // 更新当前文件大小（JSON 长度 + 换行符）
                    currentSize_ += json.size() + 1;
                    // 检查是否需要轮转文件
                    rotateIfNeeded();
                }
                // 控制台输出：保留彩色文本（原行），使用 TermColor 按级别染色后输出到 stdout
                if (console_) {
                    // 使用 TermColor 对日志行进行彩色装饰
                    std::string colored = TermColor::_decorate(line.data(), line.size());
                    // 添加换行符
                    colored.push_back('\n');
                    // 将彩色文本写入标准输出
                    std::fwrite(colored.data(), 1, colored.size(), stdout);
                    // 刷新标准输出
                    std::fflush(stdout);
                }
            }
        }
    }

    // ── 解析 trantor 文本行 → JSON ───────────────────────────────────────────
    // trantor 格式：YYYYMMDD HH:MM:SS.ffffff  threadId LEVEL  message - file:line
    static std::string toJson(const std::string& raw) {
        const char* p   = raw.c_str();
        const char* end = p + raw.size();

        std::string ts, thread, level, file, lineno, msg;

        // 1. 日期 + 时间（"YYYYMMDD HH:MM:SS.ffffff"，共 24 chars + space）
        if (end - p >= 24) {
            char date[11];
            snprintf(date, sizeof(date), "%.4s-%.2s-%.2s", p, p+4, p+6);
            p += 9; // skip "YYYYMMDD "
            const char* tEnd = (const char*)memchr(p, ' ', end - p);
            if (tEnd) {
                ts = std::string(date) + "T" + std::string(p, tEnd);
                p = tEnd + 1;
            }
        }

        // 1.5 跳过可选的 "UTC" 时区标记
        {
            const char* pp = skipSpaces(p, end);
            if (end - pp >= 3 && memcmp(pp, "UTC", 3) == 0 &&
                (pp + 3 == end || pp[3] == ' '))
                p = skipSpaces(pp + 3, end);
            else
                p = pp;
        }

        // 2. Thread id（到下一个空格）
        const char* tokEnd = nextSpace(p, end);
        thread.assign(p, tokEnd);
        p = skipSpaces(tokEnd, end);

        // 3. Level（到下一个空格或多空格）
        tokEnd = nextSpace(p, end);
        level.assign(p, tokEnd);
        trim(level);
        p = skipSpaces(tokEnd, end);

        // 4. trantor 格式：message - file:line（finish() 把 " - file:line" 追加到末尾）
        //    用 rfind 找最后一个 " - "，避免消息本身含有 " - " 被误切
        const char* lastDash = nullptr;
        for (const char* q = end - 3; q >= p; --q) {
            if (memcmp(q, " - ", 3) == 0) { lastDash = q; break; }
        }
        if (lastDash) {
            msg.assign(p, lastDash);
            trim(msg);
            std::string src(lastDash + 3, end);
            trim(src);
            size_t col = src.rfind(':');
            if (col != std::string::npos) {
                file   = src.substr(0, col);
                lineno = src.substr(col + 1);
            } else {
                file = src;
            }
        } else {
            // 没有 " - " 分隔，全部视为消息
            msg.assign(p, end);
            trim(msg);
        }

        // 6. 构建 JSON（避免引入额外依赖，手工序列化）
        std::string j;
        j.reserve(128 + msg.size());
        j += "{\"ts\":\"";       j += esc(ts);
        j += "\",\"level\":\"";  j += esc(level);
        j += "\",\"thread\":\""; j += esc(thread);
        j += "\",\"file\":\"";   j += esc(file);
        j += "\",\"line\":";
        j += (lineno.empty() ? "0" : onlyDigits(lineno) ? lineno : "0");
        j += ",\"msg\":\"";      j += esc(msg);
        j += "\"}";
        return j;
    }

    // ── 辅助函数 ──────────────────────────────────────────────────────────────
    static const char* skipSpaces(const char* p, const char* end) {
        while (p < end && *p == ' ') ++p;
        return p;
    }
    static const char* nextSpace(const char* p, const char* end) {
        while (p < end && *p != ' ') ++p;
        return p;
    }
    static void trim(std::string& s) {
        size_t a = s.find_first_not_of(" \t\r");
        size_t b = s.find_last_not_of(" \t\r");
        if (a == std::string::npos) { s.clear(); return; }
        s = s.substr(a, b - a + 1);
    }
    static bool onlyDigits(const std::string& s) {
        for (char c : s) if (c < '0' || c > '9') return false;
        return !s.empty();
    }

    // JSON 字符串转义
    bool console_{true};  // 启动阶段显示控制台，运行后关闭

    static std::string esc(const std::string& s) {
        std::string r;
        r.reserve(s.size() + 4);
        for (unsigned char c : s) {
            switch (c) {
                case '"':  r += "\\\""; break;
                case '\\': r += "\\\\"; break;
                case '\n': r += "\\n";  break;
                case '\r': r += "\\r";  break;
                case '\t': r += "\\t";  break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                        r += buf;
                    } else {
                        r += (char)c;
                    }
            }
        }
        return r;
    }
};
