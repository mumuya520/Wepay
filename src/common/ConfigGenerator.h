// WePay-Cpp — 配置文件生成器
// ConfigGenerator.h — 默认配置文件生成器
// 当 config.json 不存在时自动生成完整默认配置
#pragma once // 防止头文件重复包含
#include <string> // 字符串库
#include <fstream> // 文件流库
#include <iostream> // 输入输出库
#include <filesystem> // 文件系统库

class ConfigGenerator {
public:
    // 检查配置文件是否存在，不存在则生成默认配置
    // 返回 true 表示文件已存在或成功生成
    static bool ensureConfig(const std::string &configFile) {
        if (std::filesystem::exists(configFile)) return true;

        std::cout << "[Init] " << configFile << " 不存在，正在生成默认配置..." << std::endl;
        std::ofstream ofs(configFile);
        if (!ofs) {
            std::cerr << "[Init] 无法创建 " << configFile << std::endl;
            return false;
        }
        ofs << defaultJson();
        ofs.close();
        std::cout << "[Init] 已生成 " << configFile << "，请根据需要修改后重新启动。" << std::endl;
        return true;
    }

private:
    static std::string defaultJson() {
        std::string json = R"({
    "listeners": [
        { "address": "0.0.0.0", "port": 8088, "https": false }
    ],
    "app": {
        "threads_num": 4,
        "debug": false,
        "enable_session": false,
        "document_root": "./",
        "upload_path": "./upload",
        "enable_server_header": false,
        "enable_date_header": true,
        "reuse_port": false,
        "log": {
            "log_path": "./logs",
            "logfile_base_name": "wepay",
            "log_size_limit": 52428800,
            "log_keep_files": 5
        }
    },
    "pg": {
        "connStr": "host=127.0.0.1 port=5432 dbname=wepay user=postgres password=changeme"
    },
    "sqlite": {
        "path": "wepay.db",
        "encrypt_key": "WePay@2026!Enc#Secure$DB%Key"
    },
    "jwt": {
        "secret": "wepay-cpp-secret-change-me-in-production",
        "expire_hours": 24
    },
    "cors": {
        "allow_origins": ["*"],
        "allow_methods": ["GET","POST","PUT","DELETE","OPTIONS"],
        "allow_headers": ["*"],
        "allow_credentials": false
    },
    "rate_limit": {
        "enabled": true,
        "max_requests": 60,
        "window_seconds": 60
    },
    "cache": {
        "enabled": true,
        "type": "memory"
    },
    "mq": {
        "enabled": false,
        "type": "local"
    },
    "frontend": {
        "enabled": true,
        "dist_path": "./web",
        "spa_mode": true,
        "api_prefix": "/prod-api",
        "cache_seconds": 3600
    },
    "embedded_frontend": {
        "enabled": false,
        "spa_mode": true,
        "api_prefix": "/prod-api"
    },
    "agpayplus": {
        "enabled": false,
        "prefix": "/plus",
        "autostart": false,
        "services": []
    },
    "nginx": {
        "enabled": false,
        "exe": "./Nginx/nginx.exe",
        "work_dir": "./Nginx",
        "autostart": false
    },
    "samwaf": {
        "enabled": false,
        "exe": "./waf/samwaf.exe",
        "admin_port": 26666,
        "listen_port": 80,
        "backend_port": 8088,
        "autostart": false
    },
    "dujiao": {
        "enabled": false,
        "lib_path": "./dujiao.dll",
        "config_dir": "./dujiao",
        "prefix": "/dujiao",
        "ffi": true,
        "autostart": false,
        "admin_dist": "./dujiao-admin",
        "user_dist": "./dujiao-user"
    },
    "xpay_go": {
        "enabled": false,
        "exe": "./xpay-go/xpay.exe",
        "cfg_dir": "./xpay-go",
        "port": 8888
    },
    "gopay": {
        "enabled": false,
        "host": "127.0.0.1",
        "port": "9090"
    },
    "upay_shared": {
        "enabled": false,
        "library_path": "./upay_shared.dll",
        "base_url": "http://127.0.0.1:8090",
        "start_server": false,
        "start_cron": false,
        "prefix": "/upay",
        "static_dir": "./upay"
    },
    "wepay": {
        "frontend_url": "http://localhost:3006",
        "name": "WePay-Cpp",
        "admin_user": "admin",
        "admin_pass": "admin",
        "order_expire_minutes": 5
    },
    "oss": {
        "enabled": false,
        "provider": "minio",
        "endpoint": "http://127.0.0.1:9000",
        "access_key": "minioadmin",
        "secret_key": "minioadmin",
        "bucket": "wepay",
        "region": "us-east-1",
        "base_url": "http://127.0.0.1:9000/wepay"
    },
    "nacos": {
        "enabled": false,
        "server": "127.0.0.1:8848",
        "service_name": "wepay-cpp",
        "ip": "127.0.0.1",
        "port": 0,
        "group": "DEFAULT_GROUP",
        "namespace": "",
        "heartbeat_seconds": 5
    }
})";
#ifndef _WIN32
        // Linux: 去掉 .exe 后缀，.dll → .so
        size_t pos = 0;
        while ((pos = json.find(".exe", pos)) != std::string::npos)
            json.erase(pos, 4);
        pos = 0;
        while ((pos = json.find(".dll", pos)) != std::string::npos)
            json.replace(pos, 4, ".so");
#endif
        return json;
    }
};
