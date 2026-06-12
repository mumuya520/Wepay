#pragma once // 防止头文件重复包含
#include "IPlugin.h" // 包含插件接口定义
#include <map> // 标准映射容器库
#include <mutex> // 互斥锁库（用于线程安全）
#include <string> // 标准字符串库
#include <functional> // 函数对象库
#include <filesystem> // 文件系统库（用于目录遍历）
#include <trantor/utils/Logger.h> // Trantor 日志库

#ifdef _WIN32 // 如果是 Windows 平台
#  include <windows.h> // Windows API 头文件
   using LibHandle = HMODULE; // Windows 动态库句柄类型
#else // 否则是 Linux/Unix 平台
#  include <dlfcn.h> // 动态链接库加载头文件
   using LibHandle = void*; // Linux 动态库句柄类型
#endif

// 已加载插件信息结构体
struct LoadedPlugin {
    IPlugin* instance = nullptr; // 插件实例指针
    LibHandle handle = nullptr; // 动态库句柄
    PluginMeta meta; // 插件元信息
    bool enabled = true; // 插件是否启用
};

// 插件管理器类（单例模式）
class PluginManager {
public:
    // 获取插件管理器单例实例
    static PluginManager& instance() {
        static PluginManager pm; // 静态单例实例
        return pm;
    }

    // 扫描并加载 pluginsDir 下所有子目录中的插件
    void loadAll(const std::string& dir = "./plugins") {
        pluginsDir_ = dir; // 保存插件目录路径
        std::filesystem::create_directories(dir); // 创建目录（如果不存在）
        for (auto& entry : std::filesystem::directory_iterator(dir)) { // 遍历目录
            if (!entry.is_directory()) continue; // 跳过非目录项
            load(entry.path().filename().string()); // 加载子目录中的插件
        }
    }

    // 加载单个插件（无需重启）
    bool load(const std::string& name) {
        std::lock_guard<std::mutex> lk(mu_); // 加锁保证线程安全
        if (plugins_.count(name)) return true; // 如果已加载则直接返回

        std::string libPath = pluginsDir_ + "/" + name + "/" + name; // 构建库文件路径
#ifdef _WIN32 // Windows 平台处理
        libPath += ".dll"; // 添加 DLL 扩展名
        LibHandle h = LoadLibraryA(libPath.c_str()); // 加载 DLL 文件
        if (!h) { // 加载失败
            LOG_WARN << "[Plugin] LoadLibrary 失败: " << libPath
                     << " err=" << GetLastError(); // 记录错误信息
            return false;
        }
        auto create = (IPlugin*(*)())GetProcAddress(h, "createPlugin"); // 获取创建函数指针
        auto destroy = (void(*)(IPlugin*))GetProcAddress(h, "destroyPlugin"); // 获取销毁函数指针
#else // Linux/Unix 平台处理
        libPath += ".so"; // 添加 SO 扩展名
        LibHandle h = dlopen(libPath.c_str(), RTLD_NOW | RTLD_LOCAL); // 加载共享库
        if (!h) { // 加载失败
            LOG_WARN << "[Plugin] dlopen 失败: " << libPath
                     << " err=" << dlerror(); // 记录错误信息
            return false;
        }
        auto create = (IPlugin*(*)())dlsym(h, "createPlugin"); // 获取创建函数指针
        auto destroy = (void(*)(IPlugin*))dlsym(h, "destroyPlugin"); // 获取销毁函数指针
#endif
        IPlugin* p = nullptr; // 插件实例指针初始化
        try { // 异常处理块
            if (create) { // 如果找到 createPlugin 函数（C++ 插件）
                p = create(); // 调用创建函数创建插件实例
                p->onLoad(); // 调用插件加载回调
            } else { // 否则尝试作为 C-ABI 插件处理
                p = makeCabiPlugin(h, name); // 构造 C-ABI 适配器
                if (!p) { // 如果构造失败
                    LOG_WARN << "[Plugin] 未找到 createPlugin 或 plugin_meta_name: " << name;
                    closeLib(h); // 关闭库
                    return false;
                }
                p->onLoad(); // 调用插件加载回调
            }
        } catch (const std::exception& e) { // 捕获异常
            LOG_WARN << "[Plugin] onLoad 异常: " << name << " " << e.what(); // 记录异常信息
            if (p && destroy) destroy(p); // 如果有销毁函数则调用
            else delete p; // 否则直接删除
            closeLib(h); // 关闭库
            return false;
        }

        plugins_[name] = { p, h, p->meta(), true }; // 将插件信息存储到映射
        LOG_INFO << "[Plugin] 已加载: " << name << " v" << p->meta().version; // 记录加载成功
        return true;
    }

    // 卸载插件（无需重启）
    bool unload(const std::string& name) {
        std::lock_guard<std::mutex> lk(mu_); // 加锁保证线程安全
        auto it = plugins_.find(name); // 查找插件
        if (it == plugins_.end()) return false; // 如果未找到则返回失败

        try { it->second.instance->onUnload(); } catch (...) {} // 调用卸载回调（忽略异常）

        auto h = it->second.handle; // 获取库句柄
#ifdef _WIN32 // Windows 平台处理
        auto destroy = (void(*)(IPlugin*))GetProcAddress(h, "destroyPlugin"); // 获取销毁函数指针
#else // Linux/Unix 平台处理
        auto destroy = (void(*)(IPlugin*))dlsym(h, "destroyPlugin"); // 获取销毁函数指针
#endif
        if (destroy) destroy(it->second.instance); // 调用销毁函数销毁实例
        closeLib(h); // 关闭库
        plugins_.erase(it); // 从映射中删除
        LOG_INFO << "[Plugin] 已卸载: " << name; // 记录卸载成功
        return true;
    }

    // 热重载（替换新版本 .dll/.so 后调用）
    bool reload(const std::string& name) {
        unload(name); // 先卸载旧版本
        return load(name); // 再加载新版本
    }

    // 获取所有已加载插件信息（只读）
    std::vector<LoadedPlugin> list() const {
        std::lock_guard<std::mutex> lk(mu_); // 加锁保证线程安全
        std::vector<LoadedPlugin> out; // 输出向量
        for (auto& [k, v] : plugins_) out.push_back(v); // 遍历所有插件并添加到输出
        return out;
    }

    // 检查插件是否已加载
    bool isLoaded(const std::string& name) const {
        std::lock_guard<std::mutex> lk(mu_); // 加锁保证线程安全
        return plugins_.count(name) > 0; // 检查插件是否存在
    }

    // 触发所有插件的支付成功钩子
    void notifyPaySuccess(const std::string& orderId, const std::string& amount = "",
                          const std::string& currency = "", const std::string& extra = "") {
        std::lock_guard<std::mutex> lk(mu_); // 加锁保证线程安全
        PayEvent ev{ orderId, amount, currency, extra }; // 构造支付事件
        for (auto& [k, v] : plugins_) { // 遍历所有插件
            try { v.instance->onPaySuccess(ev); } catch (...) {} // 调用回调（忽略异常）
        }
    }

    // 获取插件目录路径
    const std::string& pluginsDir() const { return pluginsDir_; }

private:
    mutable std::mutex mu_; // 互斥锁（用于线程同步）
    std::map<std::string, LoadedPlugin> plugins_; // 已加载插件映射
    std::string pluginsDir_ = "./plugins"; // 插件目录路径

    // 从动态库中获取符号地址
    void* getSym(LibHandle h, const char* sym) {
#ifdef _WIN32 // Windows 平台处理
        return (void*)GetProcAddress(h, sym); // 获取 Windows DLL 中的函数地址
#else // Linux/Unix 平台处理
        return (void*)dlsym(h, sym); // 获取共享库中的符号地址
#endif
    }

    // 关闭动态库
    void closeLib(LibHandle h) {
#ifdef _WIN32 // Windows 平台处理
        FreeLibrary(h); // 释放 Windows DLL
#else // Linux/Unix 平台处理
        dlclose(h); // 关闭共享库
#endif
    }

    // C-ABI 插件适配器（用于 CGo / C / Rust 插件）
    // 插件需导出: plugin_meta_name/version/author/desc
    //             plugin_on_load / plugin_on_unload
    //             plugin_handle(method, subPath, body, &outBody, &outStatus)
    struct CabiPlugin : public IPlugin {
        using FnStr = const char*(*)(); // 字符串返回函数指针类型
        using FnVoid = void(*)(); // 无参无返回函数指针类型
        using FnHandle = void(*)(const char*, const char*, const char*, // HTTP 请求处理函数指针类型
                                  char**, int*);
        using FnFree = void(*)(char*); // 内存释放函数指针类型

        FnStr f_name, f_ver, f_author, f_desc; // 插件元信息函数指针
        FnVoid f_load, f_unload; // 加载卸载回调函数指针
        FnHandle f_handle; // HTTP 请求处理函数指针
        FnFree f_free = nullptr; // 可选：释放 outBody 内存的函数指针

        // 返回插件元信息
        PluginMeta meta() const override {
            return { safeStr(f_name), safeStr(f_ver), // 安全获取名称和版本
                     safeStr(f_author), safeStr(f_desc) }; // 安全获取作者和描述
        }
        // 调用加载回调
        void onLoad() override { if (f_load) f_load(); }
        // 调用卸载回调
        void onUnload() override { if (f_unload) f_unload(); }

        // 处理 HTTP 请求
        void handle(const PluginRequest& req, PluginResponse& resp) override {
            char* outBody = nullptr; // 输出响应体指针
            int outStatus = 200; // 输出 HTTP 状态码
            f_handle(req.method.c_str(), req.subPath.c_str(), // 调用 C-ABI 处理函数
                     req.body.c_str(), &outBody, &outStatus);
            resp.statusCode = outStatus; // 设置响应状态码
            if (outBody) { // 如果有响应体
                resp.body = outBody; // 设置响应体
                if (f_free) f_free(outBody); // 使用自定义释放函数
                else free(outBody); // 或使用标准 free
            }
        }

    private:
        // 安全获取字符串（处理 null 指针）
        static std::string safeStr(FnStr f) {
            if (!f) return ""; // 如果函数指针为空则返回空字符串
            const char* s = f(); // 调用函数获取字符串指针
            return s ? s : ""; // 如果指针非空则返回字符串，否则返回空字符串
        }
    };

    // 尝试构造 C-ABI 适配器
    IPlugin* makeCabiPlugin(LibHandle h, const std::string& /*name*/) {
        auto f_name = (CabiPlugin::FnStr)getSym(h, "plugin_meta_name"); // 获取名称函数指针
        if (!f_name) return nullptr; // 如果不存在则不是 C-ABI 插件

        auto* p = new CabiPlugin(); // 创建 C-ABI 适配器实例
        p->f_name = f_name; // 设置名称函数指针
        p->f_ver = (CabiPlugin::FnStr)getSym(h, "plugin_meta_version"); // 获取版本函数指针
        p->f_author = (CabiPlugin::FnStr)getSym(h, "plugin_meta_author"); // 获取作者函数指针
        p->f_desc = (CabiPlugin::FnStr)getSym(h, "plugin_meta_desc"); // 获取描述函数指针
        p->f_load = (CabiPlugin::FnVoid)getSym(h, "plugin_on_load"); // 获取加载回调函数指针
        p->f_unload = (CabiPlugin::FnVoid)getSym(h, "plugin_on_unload"); // 获取卸载回调函数指针
        p->f_handle = (CabiPlugin::FnHandle)getSym(h, "plugin_handle"); // 获取请求处理函数指针
        p->f_free = (CabiPlugin::FnFree)getSym(h, "plugin_free_str"); // 获取内存释放函数指针

        if (!p->f_handle) { delete p; return nullptr; } // 如果没有处理函数则失败
        return p; // 返回适配器实例
    }
};
