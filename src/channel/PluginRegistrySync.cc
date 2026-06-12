// WePay-Cpp — ChannelPluginRegistry::syncFromDb 实现
// 启动时将 plugin_store.installed=1 的插件加入已安装集合
#include "ChannelPlugin.h"
#include "../common/PayDb.h"
#include <iostream>

void ChannelPluginRegistry::syncFromDb() {
    auto rows = PayDb::instance().query(
        "SELECT plugin_code FROM plugin_store WHERE installed=1 AND state=1", {});
    std::lock_guard<std::mutex> lock(mutex_);
    installed_.clear();
    for (auto &r : rows) {
        auto it = r.find("plugin_code");
        if (it != r.end()) installed_.insert(it->second);
    }
    std::cout << "[PluginRegistry] 已加载 " << installed_.size() << " 个已安装插件: ";
    for (auto &n : installed_) std::cout << n << " ";
    std::cout << std::endl;
}
