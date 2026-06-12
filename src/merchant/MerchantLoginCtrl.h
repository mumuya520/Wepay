// WePay-Cpp — 商户后台: 登录/信息/密钥管理
// POST /merchant/api/auth/login              商户登录（用户名密码）
// POST /merchant/api/auth/device-key-login   设备密钥登录（EdDSA/RSA 签名）
// POST /merchant/api/auth/register           商户注册
// POST /merchant/api/auth/logout             退出登录
// GET  /merchant/api/info                    商户信息（需认证）
// POST /merchant/api/changePwd               修改密码（需认证）
// POST /merchant/api/resetKey                重置通讯密钥（需认证）
// POST /merchant/api/updateSignType          更新签名类型和公钥（需认证）
// POST /merchant/api/generateRsaKey          生成 RSA 密钥对（需认证）
#pragma once // 防止头文件重复包含
#include <drogon/HttpController.h> // Drogon HTTP 控制器
#include <ctime> // 时间库
#include <openssl/evp.h> // OpenSSL EVP 库（密钥生成）
#include <openssl/pem.h> // OpenSSL PEM 库（密钥格式）
#include <openssl/bio.h> // OpenSSL BIO 库（内存操作）
#include "../common/AjaxResult.h" // AJAX 响应结果
#include "../common/PayDb.h" // 数据库操作
#include "../common/SimpleJwt.h" // JWT 令牌生成
#include "../common/PasswordUtils.h" // 密码工具
#include "../common/DeviceKeyUtils.h" // 设备密钥工具
#include "../filters/MerchantAuthFilter.h" // 商户认证过滤器

// 商户登录控制器类
class MerchantLoginCtrl : public drogon::HttpController<MerchantLoginCtrl> {
public:
    METHOD_LIST_BEGIN // 路由列表开始
        ADD_METHOD_TO(MerchantLoginCtrl::login,          "/merchant/api/auth/login",            drogon::Post); // 用户名密码登录
        ADD_METHOD_TO(MerchantLoginCtrl::deviceKeyLogin, "/merchant/api/auth/device-key-login", drogon::Post); // 设备密钥登录
        ADD_METHOD_TO(MerchantLoginCtrl::reg,            "/merchant/api/auth/register",         drogon::Post); // 商户注册
        ADD_METHOD_TO(MerchantLoginCtrl::logout,         "/merchant/api/auth/logout",           drogon::Post); // 退出登录
        ADD_METHOD_TO(MerchantLoginCtrl::info,          "/merchant/api/info",        drogon::Get,  "MerchantAuthFilter"); // 商户信息
        ADD_METHOD_TO(MerchantLoginCtrl::changePwd,     "/merchant/api/changePwd",   drogon::Post, "MerchantAuthFilter"); // 修改密码
        ADD_METHOD_TO(MerchantLoginCtrl::resetKey,      "/merchant/api/resetKey",    drogon::Post, "MerchantAuthFilter"); // 重置密钥
        ADD_METHOD_TO(MerchantLoginCtrl::updateSignType,"/merchant/api/updateSignType", drogon::Post, "MerchantAuthFilter"); // 更新签名类型
        ADD_METHOD_TO(MerchantLoginCtrl::generateRsaKey,"/merchant/api/generateRsaKey", drogon::Post, "MerchantAuthFilter"); // 生成 RSA 密钥
    METHOD_LIST_END // 路由列表结束

    // 商户登录方法（用户名密码）
    void login(const drogon::HttpRequestPtr &req, // HTTP 请求对象
               std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        auto body = req->getJsonObject(); // 获取 JSON 请求体
        std::string username, password; // 用户名和密码
        if (body) { // 如果有 JSON 请求体
            username = (*body).get("username", "").asString(); // 从 JSON 获取用户名
            password = (*body).get("password", "").asString(); // 从 JSON 获取密码
        } else { // 否则从 URL 参数获取
            username = req->getParameter("username"); // 从参数获取用户名
            password = req->getParameter("password"); // 从参数获取密码
        }
        if (username.empty() || password.empty()) { // 检查用户名和密码是否为空
            RESP_ERR(cb, "用户名和密码不能为空"); return; // 返回错误响应
        }

        auto &db = PayDb::instance(); // 获取数据库单例
        auto mch = db.queryOne( // 查询商户信息
            "SELECT id,mch_no,username,password,salt,mch_name,state FROM merchant WHERE username=?",
            {username}); // 按用户名查询
        if (mch.empty()) { RESP_ERR(cb, "用户名或密码错误"); return; } // 商户不存在
        if (mch["state"] == "0") { RESP_ERR(cb, "账号已被禁用"); return; } // 账号被禁用

        // 验证密码: 优先用加盐哈希，兼容旧明文
        bool passOk = false; // 密码验证标志
        if (!mch["salt"].empty()) { // 如果存在盐值（加盐哈希）
            passOk = PasswordUtils::verify(password, mch["salt"], mch["password"]); // 验证加盐哈希密码
        } else { // 否则使用明文比较（兼容旧版本）
            passOk = (password == mch["password"]); // 直接比较密码
        }
        if (!passOk) { RESP_ERR(cb, "用户名或密码错误"); return; } // 密码错误

        // token sub 格式: "mch:<id>:<username>"
        std::string sub = "mch:" + mch["id"] + ":" + username; // 构建 JWT subject
        std::string token = SimpleJwt::sign(sub); // 生成 JWT token

        Json::Value data; // 响应数据
        data["token"]    = token; // 返回 token
        data["mch_no"]   = mch["mch_no"]; // 返回商户号
        data["mch_name"] = mch["mch_name"]; // 返回商户名称
        data["username"] = username; // 返回用户名
        RESP_JSON(cb, AjaxResult::success("登录成功", data)); // 返回成功响应
    }

    // 设备密钥登录方法（EdDSA/RSA 签名）
    void deviceKeyLogin(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                        std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        auto body = req->getJsonObject(); // 获取 JSON 请求体
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; } // 请求体不是 JSON
        auto &j = *body; // 引用 JSON 对象

        std::string username  = j.get("username", "").asString(); // 获取用户名
        int deviceId          = j.get("device_id", 0).asInt(); // 获取设备 ID
        std::string challenge = j.get("challenge", "").asString(); // 获取挑战值
        std::string signature = j.get("signature", "").asString(); // 获取签名

        if (username.empty() || deviceId <= 0 || challenge.empty() || signature.empty()) { // 检查参数完整性
            RESP_ERR(cb, "参数不完整"); return; // 参数不完整
        }

        auto &db = PayDb::instance(); // 获取数据库单例

        // 1. 查询商户信息
        auto mch = db.queryOne( // 查询商户
            "SELECT id,mch_no,username,mch_name,state FROM merchant WHERE username=?",
            {username}); // 按用户名查询
        if (mch.empty()) { RESP_ERR(cb, "商户不存在"); return; } // 商户不存在
        if (mch["state"] == "0") { RESP_ERR(cb, "账号已被禁用"); return; } // 账号被禁用

        int mchId = 0; // 商户 ID
        try { mchId = std::stoi(mch["id"]); } catch (...) {} // 转换商户 ID 为整数

        // 2. 查询设备密钥
        auto device = db.queryOne( // 查询设备密钥
            "SELECT id,public_key,key_type,state,last_used_at FROM device_keys "
            "WHERE id=? AND user_id=? AND user_type=2",
            {std::to_string(deviceId), mch["id"]}); // 按设备 ID 和用户 ID 查询

        if (device.empty()) { RESP_ERR(cb, "设备不存在"); return; } // 设备不存在
        if (device["state"] != "1") { RESP_ERR(cb, "设备已被撤销"); return; } // 设备已被撤销

        // 3. 验证签名
        std::string publicKey = device["public_key"]; // 获取公钥
        std::string keyType   = device["key_type"]; // 获取密钥类型

        DeviceKeyUtils::KeyType kt; // 密钥类型枚举
        if (keyType == "ed25519") kt = DeviceKeyUtils::KeyType::ED25519; // EdDSA 密钥
        else if (keyType == "rsa2048") kt = DeviceKeyUtils::KeyType::RSA_2048; // RSA-2048 密钥
        else if (keyType == "rsa4096") kt = DeviceKeyUtils::KeyType::RSA_4096; // RSA-4096 密钥
        else { RESP_ERR(cb, "不支持的密钥类型"); return; } // 不支持的密钥类型

        bool verified = DeviceKeyUtils::verifySignature(challenge, signature, publicKey, kt); // 验证签名
        if (!verified) { RESP_ERR(cb, "签名验证失败"); return; } // 签名验证失败

        // 4. 验证挑战值格式: login:<username>:<device_id>:<timestamp>
        std::string expectedPrefix = "login:" + username + ":" + std::to_string(deviceId) + ":"; // 预期前缀
        if (challenge.substr(0, expectedPrefix.size()) != expectedPrefix) { // 检查前缀
            RESP_ERR(cb, "挑战值格式错误"); return; // 挑战值格式错误
        }

        // 5. 验证时间戳（防止重放攻击，5分钟内有效）
        try { // 异常处理
            size_t pos = challenge.rfind(':'); // 找到最后一个冒号
            if (pos == std::string::npos) { RESP_ERR(cb, "挑战值格式错误"); return; } // 找不到冒号
            long long timestamp = std::stoll(challenge.substr(pos + 1)); // 提取时间戳
            long long now = std::time(nullptr) * 1000; // 当前时间（毫秒）
            if (std::abs(now - timestamp) > 300000) { // 检查时间差（5分钟 = 300000 毫秒）
                RESP_ERR(cb, "挑战值已过期"); return; // 挑战值已过期
            }
        } catch (...) { // 异常处理
            RESP_ERR(cb, "挑战值时间戳无效"); return; // 时间戳无效
        }

        // 6. 更新设备最后使用时间
        db.exec("UPDATE device_keys SET last_used_at=? WHERE id=?", // 更新最后使用时间
                {std::to_string(std::time(nullptr)), std::to_string(deviceId)}); // 当前时间戳

        // 7. 生成 token
        std::string sub = "mch:" + mch["id"] + ":" + username; // 构建 JWT subject
        std::string token = SimpleJwt::sign(sub); // 生成 JWT token

        Json::Value data; // 响应数据
        data["token"]    = token; // 返回 token
        data["mch_no"]   = mch["mch_no"]; // 返回商户号
        data["mch_name"] = mch["mch_name"]; // 返回商户名称
        data["username"] = username; // 返回用户名
        RESP_JSON(cb, AjaxResult::success("登录成功", data)); // 返回成功响应
    }

    // 商户注册方法
    void reg(const drogon::HttpRequestPtr &req, // HTTP 请求对象
             std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        auto body = req->getJsonObject(); // 获取 JSON 请求体
        if (!body) { RESP_ERR(cb, "请求体格式错误"); return; } // 请求体不是 JSON
        auto &j = *body; // 引用 JSON 对象

        std::string username = j.get("username", "").asString(); // 获取用户名
        std::string password = j.get("password", "").asString(); // 获取密码
        if (username.empty() || password.empty()) { // 检查用户名和密码是否为空
            RESP_ERR(cb, "用户名和密码不能为空"); return; // 用户名或密码为空
        }
        if (username.size() < 3 || username.size() > 32) { // 检查用户名长度
            RESP_ERR(cb, "用户名长度3-32位"); return; // 用户名长度不符合
        }
        if (password.size() < 6) { // 检查密码长度
            RESP_ERR(cb, "密码至少6位"); return; // 密码长度不符合
        }

        auto &db = PayDb::instance(); // 获取数据库单例

        // 检查是否允许开放注册
        std::string allowReg = db.getSetting("allow_merchant_register", "1"); // 获取注册开关设置
        if (allowReg != "1") { // 检查是否允许注册
            RESP_ERR(cb, "当前未开放注册，请联系管理员"); return; // 注册未开放
        }

        auto exist = db.queryOne("SELECT id FROM merchant WHERE username=?", {username}); // 检查用户名是否已存在
        if (!exist.empty()) { RESP_ERR(cb, "用户名已存在"); return; } // 用户名已存在

        std::string mchNo  = PasswordUtils::generateMchNo(); // 生成商户号
        std::string mchKey = PasswordUtils::generateKey(32); // 生成通讯密钥（32 位）
        std::string salt   = PasswordUtils::generateSalt(); // 生成密码盐值
        std::string hash   = PasswordUtils::hashPassword(password, salt); // 计算密码哈希
        long long now = std::time(nullptr); // 获取当前时间戳

        // state=1 注册即启用，直接可登录
        db.exec("INSERT INTO merchant(mch_no,username,password,salt,mch_name," // 插入商户记录
                "contact,phone,email,mch_key,rate,state,created_at,updated_at) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,1,?,?)",
                {mchNo, username, hash, salt, username, "", "", "", // 商户号、用户名、密码哈希、盐值、商户名
                 mchKey, "1.00", std::to_string(now), std::to_string(now)}); // 通讯密钥、费率、创建时间、更新时间

        // 自动为新商户绑定所有管理员启用的支付通道
        // 只绑定 state=1 的通道（管理员启用的）
        auto mchRow = db.queryOne("SELECT id FROM merchant WHERE mch_no=?", {mchNo}); // 查询新商户的 ID
        if (!mchRow.empty()) { // 如果商户创建成功
            int mchId = std::stoi(mchRow["id"]); // 获取商户 ID
            // 查询所有管理员启用的通道（state=1）
            auto channels = db.query( // 查询所有启用的支付通道
                "SELECT id FROM pay_channel WHERE state=1 ORDER BY sort_order ASC",
                {}); // 按排序顺序查询
            for (auto &ch : channels) { // 遍历每个通道
                int channelId = std::stoi(ch["id"]); // 获取通道 ID
                db.exec("INSERT INTO merchant_channel(mch_id,channel_id,rate,state) " // 为商户绑定通道
                        "VALUES(?,?,?,1)",
                        {std::to_string(mchId), std::to_string(channelId), ""}); // 商户 ID、通道 ID、费率为空（使用默认）、状态为启用
            }
        }

        RESP_MSG(cb, "注册成功，请登录"); // 返回成功消息
    }

    // 商户退出登录方法
    void logout(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        RESP_MSG(cb, "退出成功"); // 返回退出成功消息
    }

    // 获取商户信息方法
    void info(const drogon::HttpRequestPtr &req, // HTTP 请求对象
              std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        auto mch = PayDb::instance().queryOne( // 查询商户信息
            "SELECT id,mch_no,username,mch_name,contact,phone,email,mch_key," // 查询基本信息
            "sign_type,public_key,balance,frozen,total_income,rate,settle_type," // 查询签名和余额信息
            "notify_url,return_url,state,created_at FROM merchant WHERE id=?", // 查询回调和状态信息
            {mchId}); // 按商户 ID 查询
        if (mch.empty()) { RESP_ERR(cb, "商户不存在"); return; } // 商户不存在
        Json::Value data; // 响应数据
        for (auto &[k, v] : mch) data[k] = v; // 将查询结果转换为 JSON
        data["id"] = std::stoi(mch["id"]); // 将 ID 转换为整数
        RESP_OK(cb, data); // 返回商户信息
    }

    // 修改密码方法
    void changePwd(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                   std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        auto body = req->getJsonObject(); // 获取 JSON 请求体
        if (!body) { RESP_ERR(cb, "格式错误"); return; } // 请求体不是 JSON
        std::string oldPwd = (*body).get("old_password", "").asString(); // 获取原密码
        std::string newPwd = (*body).get("new_password", "").asString(); // 获取新密码
        if (newPwd.empty() || newPwd.size() < 6) { // 检查新密码长度
            RESP_ERR(cb, "新密码至少6位"); return; // 新密码长度不符合
        }

        auto &db = PayDb::instance(); // 获取数据库单例
        auto mch = db.queryOne("SELECT password,salt FROM merchant WHERE id=?", {mchId}); // 查询商户密码和盐值
        if (mch.empty()) { RESP_ERR(cb, "商户不存在"); return; } // 商户不存在

        bool passOk = !mch["salt"].empty() // 检查是否存在盐值
            ? PasswordUtils::verify(oldPwd, mch["salt"], mch["password"]) // 验证加盐哈希密码
            : (oldPwd == mch["password"]); // 或直接比较明文密码
        if (!passOk) { RESP_ERR(cb, "原密码错误"); return; } // 原密码错误

        std::string salt = PasswordUtils::generateSalt(); // 生成新盐值
        std::string hash = PasswordUtils::hashPassword(newPwd, salt); // 计算新密码哈希
        db.exec("UPDATE merchant SET password=?,salt=?,updated_at=? WHERE id=?", // 更新商户密码
                {hash, salt, std::to_string(std::time(nullptr)), mchId}); // 新密码哈希、盐值、更新时间、商户 ID
        RESP_MSG(cb, "密码修改成功"); // 返回成功消息
    }

    // 重置通讯密钥方法
    void resetKey(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                  std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        std::string newKey = PasswordUtils::generateKey(32); // 生成新的 32 位密钥
        PayDb::instance().exec("UPDATE merchant SET mch_key=?,updated_at=? WHERE id=?", // 更新商户密钥
            {newKey, std::to_string(std::time(nullptr)), mchId}); // 新密钥、更新时间、商户 ID
        Json::Value data; // 响应数据
        data["mch_key"] = newKey; // 返回新密钥
        RESP_OK(cb, data); // 返回成功响应
    }

    // 更新签名类型方法
    void updateSignType(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                        std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        std::string mchId = req->getHeader("X-Mch-Id"); // 从请求头获取商户 ID
        if (mchId.empty()) { RESP_ERR(cb, "未授权"); return; } // 商户 ID 为空
        
        auto body = req->getJsonObject(); // 获取 JSON 请求体
        if (!body) { RESP_ERR(cb, "格式错误"); return; } // 请求体不是 JSON
        
        std::string signType = (*body).get("sign_type", "MD5").asString(); // 获取签名类型（默认 MD5）
        std::string publicKey = (*body).get("public_key", "").asString(); // 获取公钥
        
        // 验证签名类型
        if (signType != "MD5" && signType != "RSA") { // 检查签名类型是否有效
            RESP_ERR(cb, "签名类型只能是 MD5 或 RSA"); return; // 签名类型无效
        }
        
        // RSA 签名必须提供公钥
        if (signType == "RSA" && publicKey.empty()) { // 如果是 RSA 签名但没有公钥
            RESP_ERR(cb, "RSA 签名必须提供公钥"); return; // 缺少公钥
        }
        
        // 如果是 MD5，清空公钥
        if (signType == "MD5") { // 如果是 MD5 签名
            publicKey = ""; // 清空公钥
        }
        
        auto &db = PayDb::instance(); // 获取数据库单例
        bool ok = db.exec( // 执行更新
            "UPDATE merchant SET sign_type=?,public_key=?,updated_at=? WHERE id=?", // 更新签名类型和公钥
            {signType, publicKey, std::to_string(std::time(nullptr)), mchId} // 签名类型、公钥、更新时间、商户 ID
        );
        
        if (!ok) { // 如果更新失败
            RESP_ERR(cb, "数据库更新失败，请检查商户ID是否正确"); // 返回错误
            return; // 返回
        }
        
        Json::Value data; // 响应数据
        data["sign_type"] = signType; // 返回签名类型
        data["public_key"] = publicKey; // 返回公钥
        RESP_OK(cb, data); // 返回成功响应
    }

    // 生成 RSA 密钥对方法
    void generateRsaKey(const drogon::HttpRequestPtr &req, // HTTP 请求对象
                        std::function<void(const drogon::HttpResponsePtr &)> &&cb) { // 响应回调函数
        // 生成 RSA-2048 密钥对
        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr); // 创建 RSA 密钥上下文
        if (!ctx) { RESP_ERR(cb, "密钥生成失败"); return; } // 上下文创建失败
        
        EVP_PKEY *pkey = nullptr; // 密钥对指针
        if (EVP_PKEY_keygen_init(ctx) <= 0 || // 初始化密钥生成
            EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) <= 0 || // 设置密钥长度为 2048 位
            EVP_PKEY_keygen(ctx, &pkey) <= 0) { // 生成密钥对
            EVP_PKEY_CTX_free(ctx); // 释放上下文
            RESP_ERR(cb, "密钥生成失败"); return; // 密钥生成失败
        }
        EVP_PKEY_CTX_free(ctx); // 释放上下文
        
        // 导出公钥（PEM 格式）
        BIO *pubBio = BIO_new(BIO_s_mem()); // 创建内存 BIO 用于公钥
        if (!PEM_write_bio_PUBKEY(pubBio, pkey)) { // 将公钥写入 BIO
            BIO_free(pubBio); // 释放 BIO
            EVP_PKEY_free(pkey); // 释放密钥对
            RESP_ERR(cb, "公钥导出失败"); return; // 公钥导出失败
        }
        
        char *pubData = nullptr; // 公钥数据指针
        long pubLen = BIO_get_mem_data(pubBio, &pubData); // 获取公钥数据和长度
        std::string publicKeyPem(pubData, pubLen); // 转换为字符串
        BIO_free(pubBio); // 释放 BIO
        
        // 导出私钥（PEM 格式）
        BIO *privBio = BIO_new(BIO_s_mem()); // 创建内存 BIO 用于私钥
        if (!PEM_write_bio_PrivateKey(privBio, pkey, nullptr, nullptr, 0, nullptr, nullptr)) { // 将私钥写入 BIO
            BIO_free(privBio); // 释放 BIO
            EVP_PKEY_free(pkey); // 释放密钥对
            RESP_ERR(cb, "私钥导出失败"); return; // 私钥导出失败
        }
        
        char *privData = nullptr; // 私钥数据指针
        long privLen = BIO_get_mem_data(privBio, &privData); // 获取私钥数据和长度
        std::string privateKeyPem(privData, privLen); // 转换为字符串
        BIO_free(privBio); // 释放 BIO
        EVP_PKEY_free(pkey); // 释放密钥对
        
        Json::Value data; // 响应数据
        data["public_key"] = publicKeyPem; // 返回公钥
        data["private_key"] = privateKeyPem; // 返回私钥
        RESP_OK(cb, data); // 返回成功响应
    }
};
