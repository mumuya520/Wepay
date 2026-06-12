// WePay-Cpp — 双后端数据库服务（SQLite + PostgreSQL）
// 支持 SQLite 默认后端和可选 PostgreSQL 后端，自动转换 SQL 占位符
// 提供单例、线程安全的数据库操作接口
#pragma once // 防止头文件重复包含
#include <sqlite3.h> // SQLite 库
#include <string> // 字符串库
#include <vector> // 向量容器
#include <unordered_map> // 哈希表
#include <mutex> // 互斥锁
#include <condition_variable> // 条件变量
#include <thread> // 线程库
#include <queue> // 队列容器
#include <atomic> // 原子操作
#include <stdexcept> // 异常库
#include <iostream> // 输入输出库
#include <fstream> // 文件流库
#include <filesystem> // 文件系统库
#include <regex> // 正则表达式库
#include <optional> // 可选值库
#include <algorithm> // 算法库
#include <cctype> // 字符类型库
#include <cstring> // C 字符串库
#include <openssl/evp.h> // OpenSSL EVP 库
#include <openssl/rand.h> // OpenSSL 随机数库
#include <trantor/utils/Logger.h> // 日志库

#ifdef WEPAY_HAS_LIBPQ
#include <libpq-fe.h> // PostgreSQL 库
#endif

#ifdef WEPAY_HAS_PG_DRIVER
#include "WpPgDriver.h" // WePay PostgreSQL 驱动
#endif

// 双后端（SQLite 默认 + 可选 PostgreSQL）数据库服务
// - 单例模式，全局唯一实例
// - 线程安全（互斥锁保护）
// - SQL 占位符使用 ? ；走 PG 时自动转换为 $1,$2,...
// - Row 列名键统一小写
class PayDb {
public:
    // 行数据类型（列名 -> 值的哈希表）
    using Row  = std::unordered_map<std::string, std::string>;
    // 行集合类型（多行数据）
    using Rows = std::vector<Row>;

    // 获取数据库单例实例
    // 返回：全局唯一的 PayDb 实例
    static PayDb &instance() {
        // 使用静态变量实现单例模式（线程安全）
        static PayDb inst;
        // 返回实例引用
        return inst;
    }

    // 设置数据库加密密钥
    // 参数 passphrase：加密密钥（空字符串表示不加密）
    // 注意：必须在 open() 之前调用
    void setEncryptionKey(const std::string &passphrase) {
        // 保存加密密钥
        encPassphrase_ = passphrase;
    }

    // 打开 SQLite 数据库
    // 参数 path：SQLite 数据库文件路径
    // 注意：必须在 main() 中调用一次
    // 加密模式下：若 path.enc 存在则解密到 path 再打开
    void open(const std::string &path) {
        // 获取 SQLite 互斥锁
        std::lock_guard<std::mutex> lk(sqliteMtx_);
        // 如果数据库已打开，直接返回
        if (sqlite_)
            return;
        // 保存数据库路径
        dbPath_ = path;
        // 加密文件路径（.enc 后缀）
        encPath_ = path + ".enc";

        // 如果启用了加密且加密文件存在
        if (!encPassphrase_.empty() && std::filesystem::exists(encPath_)) {
            // 解密 .enc 文件到 .db 文件
            if (!decryptFile(encPath_, dbPath_, encPassphrase_)) {
                // 如果解密失败，抛出异常
                throw std::runtime_error("[PayDb] 解密数据库失败，密钥错误或文件损坏");
            }
            // 记录解密成功的日志
            LOG_INFO << "[PayDb] SQLite 已从加密文件解密: " << encPath_;
        }

        // 打开 SQLite 数据库
        if (sqlite3_open(path.c_str(), &sqlite_) != SQLITE_OK)
            // 如果打开失败，抛出异常
            throw std::runtime_error(std::string("PayDb open failed: ") + sqlite3_errmsg(sqlite_));
        // 设置日志模式为 WAL（Write-Ahead Logging）
        sqlite3_exec(sqlite_, "PRAGMA journal_mode=WAL;",  nullptr, nullptr, nullptr);
        // 设置同步模式为 NORMAL（平衡性能和安全性）
        sqlite3_exec(sqlite_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
        // 设置忙超时为 5 秒
        sqlite3_busy_timeout(sqlite_, 5000);

        // 如果启用了加密
        if (!encPassphrase_.empty()) {
            // 标记为已加密
            encrypted_ = true;
            // 记录加密模式的日志
            LOG_INFO << "[PayDb] SQLite opened (AES-256-CBC encrypted): " << path;
        } else {
            // 记录未加密模式的日志
            LOG_INFO << "[PayDb] SQLite opened: " << path;
        }
    }

    // 手动将当前 SQLite 数据库加密落盘到 .enc 文件
    // 返回：true 表示加密成功，false 表示加密失败或未启用加密
    bool flushEncrypt() {
        // 检查是否启用了加密
        if (!encrypted_ || encPassphrase_.empty())
            // 如果未启用加密，返回 false
            return false;
        // 获取 SQLite 互斥锁
        std::lock_guard<std::mutex> lk(sqliteMtx_);
        // 先将 WAL 日志 checkpoint 到主库文件
        sqlite3_wal_checkpoint_v2(sqlite_, nullptr, SQLITE_CHECKPOINT_FULL, nullptr, nullptr);
        // 加密数据库文件
        return encryptFile(dbPath_, encPath_, encPassphrase_);
    }

    // 连接 PostgreSQL 数据库
    // 参数 connStr：PostgreSQL 连接字符串
    // 返回：true 表示连接成功，false 表示连接失败
    // 优先尝试自研驱动，失败则降级到官方 libpq
    bool connectPg(const std::string &connStr) {
        // 保存连接字符串（供降级时重新连接）
        pgConnStr_ = connStr;

// 如果编译了自研 PG 驱动
#ifdef WEPAY_HAS_PG_DRIVER
        // 记录尝试自研驱动的日志
        LOG_INFO << "[PayDb] 尝试自研 PG 驱动...";
        // 初始化自研驱动
        if (WpPgDriver::instance().init(connStr)) {
            // 设置后端为自研驱动
            pgBackend_ = PgBackend::DRIVER;
            // 重置驱动失败计数
            driverFailCount_ = 0;
            // 记录成功的日志
            LOG_INFO << "[PayDb] ✅ PG后端: 自研驱动 (连接池+燔断器+异步写)";
            // 启动异步写回线程
            startSyncWorker();
            // 返回成功
            return true;
        }
        // 记录自研驱动初始化失败的日志
        LOG_WARN << "[PayDb] 自研驱动初始化失败，尝试官方 libpq 降级...";
#endif

// 如果编译了官方 libpq
#ifdef WEPAY_HAS_LIBPQ
        {
            // 获取 PG 互斥锁
            std::lock_guard<std::mutex> lk(pgMtx_);
            // 使用官方 libpq 连接 PostgreSQL
            pg_ = PQconnectdb(connStr.c_str());
            // 检查连接是否成功
            if (!pg_ || PQstatus(pg_) != CONNECTION_OK) {
                // 记录连接失败的日志
                LOG_ERROR << "[PayDb] 官方 libpq 连接失败: "
                          << (pg_ ? PQerrorMessage(pg_) : "null");
                // 如果连接对象存在，关闭连接
                if (pg_) {
                    PQfinish(pg_);
                    pg_ = nullptr;
                }
                // 返回失败
                return false;
            }
            // 设置后端为官方 libpq
            pgBackend_ = PgBackend::LIBPQ;
            // 记录成功的日志
            LOG_INFO << "[PayDb] ✅ PG后端: 官方 libpq (单连接)";
        }
        // 启动异步写回线程
        startSyncWorker();
        // 返回成功
        return true;
// 如果未编译 libpq
#else
        // 避免未使用参数的警告
        (void)connStr;
        // 记录 libpq 未编译的日志
        LOG_WARN << "[PayDb] libpq 未编译，跳过 PG";
        // 返回失败
        return false;
#endif
    }

    // 检查是否仅使用 SQLite（未连接 PG）
    // 返回：true 表示仅使用 SQLite，false 表示已连接 PG
    bool isUsingSqlite() const {
        return pgBackend_ == PgBackend::NONE;
    }

    // 检查是否已连接 PostgreSQL
    // 返回：true 表示已连接 PG，false 表示仅使用 SQLite
    bool hasPg() const {
        return pgBackend_ != PgBackend::NONE;
    }

    // 获取数据库后端信息
    // 返回：后端类型的描述字符串
    std::string backendInfo() const {
        // 根据后端类型返回描述信息
        switch (pgBackend_) {
        case PgBackend::DRIVER:
            // 自研驱动后端
            return "postgresql [自研驱动] + sqlite (异步从)";
        case PgBackend::LIBPQ:
            // 官方 libpq 后端
            return "postgresql [官方libpq] + sqlite (异步从)";
        default:
            // 仅 SQLite 后端
            return "sqlite";
        }
    }

    // 刷新异步写回队列
    // 等待所有待写回任务执行完毕（最多等待 3 秒）
    void flushSync() {
        // 检查异步线程是否运行
        if (!syncRunning_.load())
            // 如果未运行，直接返回
            return;
        // 通知异步线程处理队列
        syncCv_.notify_one();
        // 最多等待 3000 毫秒
        for (int i = 0; i < 3000; ++i) {
            {
                // 获取队列锁
                std::lock_guard<std::mutex> lk(syncMtx_);
                // 检查队列是否为空
                if (syncQueue_.empty())
                    // 如果队列为空，返回成功
                    return;
            }
            // 等待 1 毫秒后重试
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // 如果超时，记录警告日志
        LOG_WARN << "[PayDb][Sync] flushSync 超时，队列未完全消费";
    }

    // 停止异步写回线程
    // 会先 drain 剩余任务，然后停止线程
    void stopSync() {
        stopSyncWorker();
    }

    // ── 通用接口（自动路由 sqlite 或 pg） ─────────────────────────────
    // PG 模式: PG 为主库，SQLite 为从库（双写，读走 PG）

    // 执行 SQL 语句（INSERT/UPDATE/DELETE）
    // 参数 sql：SQL 语句（使用 ? 作为占位符）
    // 参数 params：参数列表
    // 返回：true 表示执行成功，false 表示执行失败
    bool exec(const std::string &sql, const std::vector<std::string> &params = {}) {
        // 如果后端是自研驱动
        if (pgBackend_ == PgBackend::DRIVER) {
// 如果编译了自研驱动
#ifdef WEPAY_HAS_PG_DRIVER
            // 将 SQL 转换为 PG 方言
            std::string pgSql = toPgPlaceholders(dialectifyForPg(sql));
            // 执行 SQL
            bool ok = WpPgDriver::instance().exec(pgSql, params);
            // 如果执行失败
            if (!ok)
                // 处理驱动失败
                onDriverFailure();
            else {
                // 如果执行成功
                // 重置失败计数
                if (driverFailCount_ > 0)
                    driverFailCount_ = 0;
                // 同步到 SQLite
                syncToSqlite(sql, params);
            }
            // 返回执行结果
            return ok;
#endif
        }
        // 如果后端是官方 libpq
        if (pgBackend_ == PgBackend::LIBPQ) {
            // 执行 PG SQL
            bool ok = execPg(sql, params);
            // 如果执行成功
            if (ok)
                // 同步到 SQLite
                syncToSqlite(sql, params);
            // 返回执行结果
            return ok;
        }
        // 如果仅使用 SQLite，直接执行
        return execSqliteDirect(sql, params);
    }

    // 仅写 PG 主库，不同步从库
    // 用于 migration 等自行控制从库写入的场景
    // 参数 sql：SQL 语句
    // 参数 params：参数列表
    // 返回：true 表示执行成功，false 表示执行失败
    bool execPgOnly(const std::string &sql, const std::vector<std::string> &params = {}) {
        // 如果后端是自研驱动
        if (pgBackend_ == PgBackend::DRIVER) {
// 如果编译了自研驱动
#ifdef WEPAY_HAS_PG_DRIVER
            // 将 SQL 转换为 PG 方言
            std::string pgSql = toPgPlaceholders(dialectifyForPg(sql));
            // 执行 SQL
            bool ok = WpPgDriver::instance().exec(pgSql, params);
            // 如果执行失败
            if (!ok)
                // 处理驱动失败
                onDriverFailure();
            // 返回执行结果
            return ok;
#endif
        }
        // 如果后端是官方 libpq
        if (pgBackend_ == PgBackend::LIBPQ)
            // 执行 PG SQL
            return execPg(sql, params);
        // 如果仅使用 SQLite，直接执行
        return execSqliteDirect(sql, params);
    }

    // 查询 SQL 语句（SELECT）
    // 参数 sql：SQL 语句（使用 ? 作为占位符）
    // 参数 params：参数列表
    // 返回：查询结果（行集合）
    Rows query(const std::string &sql, const std::vector<std::string> &params = {}) {
        // 如果后端是自研驱动
        if (pgBackend_ == PgBackend::DRIVER) {
// 如果编译了自研驱动
#ifdef WEPAY_HAS_PG_DRIVER
            // 将 SQL 转换为 PG 方言
            std::string pgSql = toPgPlaceholders(dialectifyForPg(sql));
            // 执行查询并返回结果
            return WpPgDriver::instance().query(pgSql, params);
#endif
        }
        // 如果后端是官方 libpq
        if (pgBackend_ == PgBackend::LIBPQ)
            // 执行 PG 查询
            return queryPg(sql, params);
        // 如果仅使用 SQLite，直接查询
        return querySqliteDirect(sql, params);
    }

    // 查询单行数据
    // 参数 sql：SQL 语句
    // 参数 params：参数列表
    // 返回：单行数据（如果无结果返回空 Row）
    Row queryOne(const std::string &sql, const std::vector<std::string> &params = {}) {
        // 执行查询
        auto rows = query(sql, params);
        // 如果结果为空，返回空 Row
        return rows.empty() ? Row{} : rows[0];
    }

    // 查询单个标量值
    // 参数 sql：SQL 语句
    // 参数 params：参数列表
    // 参数 def：默认值（如果无结果返回此值）
    // 返回：单个值字符串
    std::string queryScalar(const std::string &sql,
                            const std::vector<std::string> &params = {},
                            const std::string &def = "") {
        // 查询单行数据
        auto row = queryOne(sql, params);
        // 如果结果为空，返回默认值
        if (row.empty())
            return def;
        // 返回第一列的值
        return row.begin()->second;
    }

    // ── SQLite 直连（DatabaseInit 与 NotifyTaskService 用，绕过 PG 路由） ─

    // 直接执行 SQLite SQL 语句（绕过 PG 路由）
    // 参数 sql：SQL 语句
    // 参数 params：参数列表
    // 返回：true 表示执行成功，false 表示执行失败
    bool execSqliteDirect(const std::string &sql,
                          const std::vector<std::string> &params = {}) {
        // 获取 SQLite 互斥锁
        std::lock_guard<std::mutex> lk(sqliteMtx_);
        // 准备 SQL 语句
        sqlite3_stmt *stmt = prepareSqlite(sql);
        // 如果准备失败
        if (!stmt) {
            // ADD COLUMN: 列已存在不算错误（幂等迁移）
            if (containsCi(sql, "ALTER TABLE") && containsCi(sql, "ADD COLUMN")
                && containsCi(sqlite3_errmsg(sqlite_), "duplicate column")) {
                // 列已存在，返回成功
                return true;
            }
            // 其他错误，返回失败
            return false;
        }
        // 绑定参数
        bindSqliteParams(stmt, params);
        // 执行语句
        int rc = sqlite3_step(stmt);
        // 释放语句
        sqlite3_finalize(stmt);
        // 检查执行结果
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            // 记录错误日志
            LOG_ERROR << "[PayDb][SQLite] exec rc=" << rc
                      << " err=" << sqlite3_errmsg(sqlite_)
                      << " SQL=" << sql;
            // 返回失败
            return false;
        }
        // 返回成功
        return true;
    }

    // 直接查询 SQLite 数据（绕过 PG 路由）
    // 参数 sql：SQL 语句
    // 参数 params：参数列表
    // 返回：查询结果（行集合）
    Rows querySqliteDirect(const std::string &sql,
                           const std::vector<std::string> &params = {}) {
        // 获取 SQLite 互斥锁
        std::lock_guard<std::mutex> lk(sqliteMtx_);
        // 结果集合
        Rows result;
        // 准备 SQL 语句
        sqlite3_stmt *stmt = prepareSqlite(sql);
        // 如果准备失败，返回空结果
        if (!stmt)
            return result;
        // 绑定参数
        bindSqliteParams(stmt, params);
        // 获取列数
        int colCount = sqlite3_column_count(stmt);
        // 逐行读取结果
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            // 创建行数据
            Row row;
            // 遍历每一列
            for (int i = 0; i < colCount; ++i) {
                // 获取列名
                std::string col = sqlite3_column_name(stmt, i);
                // 获取列值
                const char *val = (const char *)sqlite3_column_text(stmt, i);
                // 添加到行数据
                row[col] = val ? val : "";
            }
            // 添加到结果集合
            result.push_back(std::move(row));
        }
        // 释放语句
        sqlite3_finalize(stmt);
        // 返回结果
        return result;
    }

    // 兼容别名（早期代码用 queryAll）
    // 参数 sql：SQL 语句
    // 参数 params：参数列表
    // 返回：查询结果（行集合）
    Rows queryAll(const std::string &sql, const std::vector<std::string> &params = {}) {
        // 调用 query 方法
        return query(sql, params);
    }

    // 获取最后插入的行 ID
    // 返回：最后插入行的 ID
    long long lastInsertId() {
        // 获取 SQLite 互斥锁
        std::lock_guard<std::mutex> lk(sqliteMtx_);
        // 返回最后插入的行 ID
        return sqlite_ ? sqlite3_last_insert_rowid(sqlite_) : 0;
    }

    // ── setting 表快捷访问 ───────────────────────────────────────

    // 获取设置值
    // 参数 key：设置键
    // 参数 def：默认值
    // 返回：设置值
    std::string getSetting(const std::string &key, const std::string &def = "") {
        // 查询设置表
        auto row = queryOne("SELECT vvalue FROM setting WHERE vkey=?", {key});
        // 如果结果为空，返回默认值
        return row.empty() ? def : row["vvalue"];
    }

    // 设置设置值
    // 参数 key：设置键
    // 参数 val：设置值
    void setSetting(const std::string &key, const std::string &val) {
        // SQLite 与 PG 都接受 INSERT ... ON CONFLICT 语法
        if (hasPg()) {
            // 使用 PG 的 ON CONFLICT 语法
            exec("INSERT INTO setting(vkey,vvalue) VALUES(?,?) "
                 "ON CONFLICT(vkey) DO UPDATE SET vvalue=EXCLUDED.vvalue", {key, val});
        } else {
            // 使用 SQLite 的 INSERT OR REPLACE 语法
            execSqliteDirect("INSERT OR REPLACE INTO setting(vkey,vvalue) VALUES(?,?)", {key, val});
        }
    }

    // SQL 字符串转义（用于 LIKE 参数等）
    // 参数 s：原始字符串
    // 返回：转义后的字符串
    static std::string escape(const std::string &s) {
        // 创建输出字符串
        std::string out;
        // 预分配空间
        out.reserve(s.size());
        // 遍历每个字符
        for (char c : s) {
            // 如果是单引号，添加两个单引号
            if (c == '\'')
                out += '\'';
            // 添加字符
            out += c;
        }
        // 返回转义后的字符串
        return out;
    }

    // 处理驱动失败（自研驱动连续失败后调用）
    // 如果失败次数超过阈值，自动切换到官方 libpq
    void onDriverFailure() {
        // 增加失败计数
        ++driverFailCount_;
        // 检查是否超过阈值
        if (driverFailCount_ >= DRIVER_FAIL_THRESHOLD) {
// 如果编译了官方 libpq
#ifdef WEPAY_HAS_LIBPQ
            // 如果当前后端是自研驱动
            if (pgBackend_ == PgBackend::DRIVER) {
                // 记录切换的日志
                LOG_WARN << "[PayDb] ⚠️ 自研驱动连续失败 " << driverFailCount_
                         << " 次，切换到官方 libpq";
                // 获取 PG 互斥锁
                std::lock_guard<std::mutex> lk(pgMtx_);
                // 使用官方 libpq 重新连接
                pg_ = PQconnectdb(pgConnStr_.c_str());
                // 检查连接是否成功
                if (pg_ && PQstatus(pg_) == CONNECTION_OK) {
                    // 设置后端为官方 libpq
                    pgBackend_ = PgBackend::LIBPQ;
                    // 重置失败计数
                    driverFailCount_ = 0;
                    // 记录成功的日志
                    LOG_INFO << "[PayDb] ✅ PG后端切换为: 官方 libpq";
                } else {
                    // 连接失败，记录错误日志
                    LOG_ERROR << "[PayDb] 官方 libpq 也连接失败，降级到 SQLite";
                    // 关闭连接
                    if (pg_) {
                        PQfinish(pg_);
                        pg_ = nullptr;
                    }
                    // 设置后端为 SQLite
                    pgBackend_ = PgBackend::NONE;
                }
            }
// 如果未编译 libpq
#else
            // 记录警告日志
            LOG_WARN << "[PayDb] 自研驱动失败且未编译 libpq，降级到 SQLite";
            // 设置后端为 SQLite
            pgBackend_ = PgBackend::NONE;
#endif
        }
    }

private:
    enum class PgBackend { NONE, DRIVER, LIBPQ };
    PgBackend pgBackend_    = PgBackend::NONE;
    int       driverFailCount_ = 0;
    static constexpr int DRIVER_FAIL_THRESHOLD = 5;
    std::string pgConnStr_;   // 保存连接字符串，供降级时重新连接 libpq

    sqlite3 *sqlite_ = nullptr;
#ifdef WEPAY_HAS_LIBPQ
    PGconn *pg_ = nullptr;
#else
    void *pg_ = nullptr;
#endif
    std::mutex sqliteMtx_;     // SQLite 专用锁
    std::mutex pgMtx_;         // PG 专用锁（与 SQLite 互不阻塞）

    // ── 加密相关 ─────────────────────────────────────────────────
    std::string encPassphrase_;         // 加密密钥（空=不加密）
    std::string dbPath_;                // 明文 .db 路径
    std::string encPath_;               // 加密 .enc 路径
    bool encrypted_ = false;            // 是否启用加密

    // ── 异步写回队列 ─────────────────────────────────────
    static constexpr size_t SYNC_QUEUE_LIMIT = 10000;   // 队列上限
    static constexpr int    SYNC_BATCH_SIZE  = 200;     // 每批最多条数

    struct SyncTask {
        std::string sql;
        std::vector<std::string> params;
    };
    std::queue<SyncTask>    syncQueue_;
    std::mutex              syncMtx_;       // 队列锁（轻量，不持有 IO）
    std::condition_variable syncCv_;
    std::thread             syncThread_;
    std::atomic<bool>       syncRunning_{false};
    std::atomic<uint64_t>   syncOkCount_{0};    // 成功计数
    std::atomic<uint64_t>   syncErrCount_{0};   // 失败计数
    std::atomic<uint64_t>   syncDropCount_{0};  // 丢弃计数

    void startSyncWorker() {
        if (syncRunning_.load()) return;
        syncRunning_ = true;
        syncThread_ = std::thread([this] {
            LOG_INFO << "[PayDb][Sync] 异步写回线程已启动 (queue_limit="
                      << SYNC_QUEUE_LIMIT << ", batch=" << SYNC_BATCH_SIZE << ")";
            uint64_t lastErrLog = 0; // 错误日志限流
            while (true) {
                std::vector<SyncTask> batch;
                {
                    std::unique_lock<std::mutex> lk(syncMtx_);
                    syncCv_.wait_for(lk, std::chrono::milliseconds(100), [this] {
                        return !syncQueue_.empty() || !syncRunning_.load();
                    });
                    int n = 0;
                    while (!syncQueue_.empty() && n < SYNC_BATCH_SIZE) {
                        batch.push_back(std::move(syncQueue_.front()));
                        syncQueue_.pop();
                        ++n;
                    }
                    if (batch.empty() && !syncRunning_.load()) break;
                }
                if (batch.empty()) continue;
                // 批量执行（事务包裹提升 SQLite 写入性能）
                {
                    std::lock_guard<std::mutex> lk(sqliteMtx_);
                    execSqliteRaw("BEGIN");
                    for (auto &t : batch) {
                        bool ok = execSqliteRaw(t.sql, t.params);
                        if (ok) {
                            syncOkCount_.fetch_add(1, std::memory_order_relaxed);
                        } else {
                            uint64_t ec = syncErrCount_.fetch_add(1, std::memory_order_relaxed) + 1;
                            // 每 100 次失败才打印一次，避免日志洪泛
                            if (ec <= 5 || ec % 100 == 0) {
                                LOG_ERROR << "[PayDb][Sync] 写回失败(#" << ec << "): "
                                          << t.sql.substr(0, 120);
                            }
                        }
                    }
                    execSqliteRaw("COMMIT");
                }
            }
            drainSyncQueue();
            LOG_INFO << "[PayDb][Sync] 异步线程停止 (ok=" << syncOkCount_.load()
                      << " err=" << syncErrCount_.load()
                      << " drop=" << syncDropCount_.load() << ")";
        });
    }

    void stopSyncWorker() {
        if (!syncRunning_.load()) return;
        syncRunning_ = false;
        syncCv_.notify_one();
        if (syncThread_.joinable()) syncThread_.join();
    }

    void drainSyncQueue() {
        std::lock_guard<std::mutex> qlk(syncMtx_);
        if (syncQueue_.empty()) return;
        std::lock_guard<std::mutex> slk(sqliteMtx_);
        execSqliteRaw("BEGIN");
        while (!syncQueue_.empty()) {
            auto &t = syncQueue_.front();
            execSqliteRaw(t.sql, t.params);
            syncQueue_.pop();
        }
        execSqliteRaw("COMMIT");
    }

    // PG 写成功后入队，后台线程异步写回 SQLite
    void syncToSqlite(const std::string &sql, const std::vector<std::string> &params) {
        std::string sqliteSql = pgToSqlite(sql);
        {
            std::lock_guard<std::mutex> lk(syncMtx_);
            if (syncQueue_.size() >= SYNC_QUEUE_LIMIT) {
                // 队列已满，丢弃最旧任务（保护内存）
                syncQueue_.pop();
                uint64_t dc = syncDropCount_.fetch_add(1, std::memory_order_relaxed) + 1;
                if (dc <= 3 || dc % 1000 == 0) {
                    LOG_WARN << "[PayDb][Sync] 队列已满(" << SYNC_QUEUE_LIMIT
                              << "), 丢弃旧任务 (#" << dc << ")";
                }
            }
            syncQueue_.push({std::move(sqliteSql), params});
        }
        syncCv_.notify_one();
    }

    // 不加锁的 SQLite 执行（调用方已持有 sqliteMtx_）
    bool execSqliteRaw(const std::string &sql,
                       const std::vector<std::string> &params = {}) {
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(sqlite_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
            return false;
        for (size_t i = 0; i < params.size(); ++i)
            sqlite3_bind_text(stmt, (int)(i + 1), params[i].c_str(), -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return (rc == SQLITE_DONE || rc == SQLITE_ROW);
    }

    // PG SQL → SQLite 兼容（反向转换）
    static std::string pgToSqlite(const std::string &sql) {
        std::string s = sql;
        // ON CONFLICT(...) DO UPDATE SET ... → INSERT OR REPLACE 不好反转
        // 但 ON CONFLICT ... DO NOTHING → INSERT OR IGNORE
        {
            // 简单情况: INSERT INTO ... ON CONFLICT DO NOTHING
            std::string tag = " ON CONFLICT DO NOTHING";
            auto pos = s.find(tag);
            if (pos != std::string::npos) {
                s.erase(pos, tag.size());
                // INSERT INTO → INSERT OR IGNORE INTO
                auto ipos = s.find("INSERT INTO");
                if (ipos != std::string::npos)
                    s.replace(ipos, 11, "INSERT OR IGNORE INTO");
            }
        }
        // ON CONFLICT(xxx) DO NOTHING
        {
            std::regex re(R"(\s+ON\s+CONFLICT\s*\([^)]*\)\s+DO\s+NOTHING)", std::regex::icase);
            if (std::regex_search(s, re)) {
                s = std::regex_replace(s, re, "");
                auto ipos = s.find("INSERT INTO");
                if (ipos != std::string::npos)
                    s.replace(ipos, 11, "INSERT OR IGNORE INTO");
            }
        }
        // ON CONFLICT(...) DO UPDATE SET ... → INSERT OR REPLACE INTO
        {
            std::regex re(R"(\s+ON\s+CONFLICT\s*\([^)]*\)\s+DO\s+UPDATE\s+SET\s+.*$)", std::regex::icase);
            if (std::regex_search(s, re)) {
                s = std::regex_replace(s, re, "");
                auto ipos = s.find("INSERT INTO");
                if (ipos != std::string::npos)
                    s.replace(ipos, 11, "INSERT OR REPLACE INTO");
            }
        }
        // BIGSERIAL PRIMARY KEY → INTEGER PRIMARY KEY AUTOINCREMENT
        {
            std::regex re(R"(\bBIGSERIAL\s+PRIMARY\s+KEY\b)", std::regex::icase);
            s = std::regex_replace(s, re, "INTEGER PRIMARY KEY AUTOINCREMENT");
        }
        // SERIAL PRIMARY KEY → INTEGER PRIMARY KEY AUTOINCREMENT
        {
            std::regex re(R"(\bSERIAL\s+PRIMARY\s+KEY\b)", std::regex::icase);
            s = std::regex_replace(s, re, "INTEGER PRIMARY KEY AUTOINCREMENT");
        }
        // BIGINT → INTEGER (SQLite 不区分)
        // 不转换，SQLite 接受 BIGINT
        return s;
    }

    PayDb() = default;
    ~PayDb() {
        stopSyncWorker();
        if (sqlite_ && encrypted_) {
            // checkpoint WAL → 主文件
            sqlite3_wal_checkpoint_v2(sqlite_, nullptr, SQLITE_CHECKPOINT_FULL, nullptr, nullptr);
            sqlite3_close(sqlite_);
            sqlite_ = nullptr;
            // 加密落盘
            if (encryptFile(dbPath_, encPath_, encPassphrase_)) {
                // 删除明文 .db 及 WAL/SHM
                std::error_code ec;
                std::filesystem::remove(dbPath_, ec);
                std::filesystem::remove(dbPath_ + "-wal", ec);
                std::filesystem::remove(dbPath_ + "-shm", ec);
                LOG_INFO << "[PayDb] 数据库已加密保存: " << encPath_;
            } else {
                LOG_ERROR << "[PayDb] 警告: 加密落盘失败，明文文件保留: " << dbPath_;
            }
        } else {
            if (sqlite_) sqlite3_close(sqlite_);
        }
#ifdef WEPAY_HAS_LIBPQ
        if (pg_) PQfinish(pg_);
#endif
    }

    // ── AES-256-CBC + PBKDF2(SHA-256, 10000轮) 文件加解密 ────────
    static constexpr int PBKDF2_ITERATIONS = 10000;
    static constexpr int AES_KEY_LEN = 32;   // 256 bits
    static constexpr int AES_IV_LEN  = 16;   // 128 bits
    static constexpr int SALT_LEN    = 16;

    // 加密文件: [salt(16)] [iv(16)] [ciphertext...] 
    static bool encryptFile(const std::string &plainPath,
                            const std::string &encPath,
                            const std::string &passphrase) {
        // 读入明文
        std::ifstream fin(plainPath, std::ios::binary);
        if (!fin) return false;
        std::vector<unsigned char> plainData(
            (std::istreambuf_iterator<char>(fin)),
            std::istreambuf_iterator<char>());
        fin.close();

        // 生成随机 salt 和 iv
        unsigned char salt[SALT_LEN], iv[AES_IV_LEN];
        if (RAND_bytes(salt, SALT_LEN) != 1) return false;
        if (RAND_bytes(iv, AES_IV_LEN) != 1) return false;

        // PBKDF2 派生密钥
        unsigned char key[AES_KEY_LEN];
        if (PKCS5_PBKDF2_HMAC(passphrase.c_str(), (int)passphrase.size(),
                               salt, SALT_LEN, PBKDF2_ITERATIONS,
                               EVP_sha256(), AES_KEY_LEN, key) != 1)
            return false;

        // AES-256-CBC 加密
        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return false;
        bool ok = false;
        std::vector<unsigned char> cipherData(plainData.size() + AES_IV_LEN + 32);
        int outLen = 0, finalLen = 0;
        do {
            if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, key, iv) != 1) break;
            if (EVP_EncryptUpdate(ctx, cipherData.data(), &outLen,
                                  plainData.data(), (int)plainData.size()) != 1) break;
            if (EVP_EncryptFinal_ex(ctx, cipherData.data() + outLen, &finalLen) != 1) break;
            ok = true;
        } while (false);
        EVP_CIPHER_CTX_free(ctx);
        // 清除密钥
        OPENSSL_cleanse(key, AES_KEY_LEN);
        if (!ok) return false;

        int totalCipher = outLen + finalLen;

        // 写入加密文件: salt + iv + ciphertext
        std::string tmpPath = encPath + ".tmp";
        std::ofstream fout(tmpPath, std::ios::binary | std::ios::trunc);
        if (!fout) return false;
        fout.write((const char*)salt, SALT_LEN);
        fout.write((const char*)iv, AES_IV_LEN);
        fout.write((const char*)cipherData.data(), totalCipher);
        fout.close();
        if (!fout) { std::filesystem::remove(tmpPath); return false; }

        // 原子替换
        std::error_code ec;
        std::filesystem::rename(tmpPath, encPath, ec);
        if (ec) { std::filesystem::remove(tmpPath); return false; }
        return true;
    }

    // 解密文件
    static bool decryptFile(const std::string &encPath,
                            const std::string &plainPath,
                            const std::string &passphrase) {
        std::ifstream fin(encPath, std::ios::binary);
        if (!fin) return false;
        std::vector<unsigned char> fileData(
            (std::istreambuf_iterator<char>(fin)),
            std::istreambuf_iterator<char>());
        fin.close();

        if (fileData.size() < (size_t)(SALT_LEN + AES_IV_LEN + 1))
            return false;

        const unsigned char *salt = fileData.data();
        const unsigned char *iv   = fileData.data() + SALT_LEN;
        const unsigned char *cipher = fileData.data() + SALT_LEN + AES_IV_LEN;
        int cipherLen = (int)(fileData.size() - SALT_LEN - AES_IV_LEN);

        // PBKDF2 派生密钥
        unsigned char key[AES_KEY_LEN];
        if (PKCS5_PBKDF2_HMAC(passphrase.c_str(), (int)passphrase.size(),
                               salt, SALT_LEN, PBKDF2_ITERATIONS,
                               EVP_sha256(), AES_KEY_LEN, key) != 1)
            return false;

        // AES-256-CBC 解密
        EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
        if (!ctx) { OPENSSL_cleanse(key, AES_KEY_LEN); return false; }
        std::vector<unsigned char> plainData(cipherLen + 32);
        int outLen = 0, finalLen = 0;
        bool ok = false;
        do {
            if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, key, iv) != 1) break;
            if (EVP_DecryptUpdate(ctx, plainData.data(), &outLen,
                                  cipher, cipherLen) != 1) break;
            if (EVP_DecryptFinal_ex(ctx, plainData.data() + outLen, &finalLen) != 1) break;
            ok = true;
        } while (false);
        EVP_CIPHER_CTX_free(ctx);
        OPENSSL_cleanse(key, AES_KEY_LEN);
        if (!ok) return false;

        int totalPlain = outLen + finalLen;
        std::ofstream fout(plainPath, std::ios::binary | std::ios::trunc);
        if (!fout) return false;
        fout.write((const char*)plainData.data(), totalPlain);
        fout.close();
        return fout.good();
    }

    sqlite3_stmt *prepareSqlite(const std::string &sql) {
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(sqlite_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            LOG_ERROR << "[PayDb][SQLite] prepare failed: " << sqlite3_errmsg(sqlite_)
                      << " SQL=" << sql;
            return nullptr;
        }
        return stmt;
    }

    void bindSqliteParams(sqlite3_stmt *stmt, const std::vector<std::string> &params) {
        for (size_t i = 0; i < params.size(); ++i) {
            sqlite3_bind_text(stmt, (int)(i + 1), params[i].c_str(), -1, SQLITE_TRANSIENT);
        }
    }

    // ── SQL 方言辅助 ─────────────────────────────────────────────
    static bool containsCi(const std::string &s, const std::string &needle) {
        auto cmp = [](char a, char b) {
            return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
        };
        return std::search(s.begin(), s.end(), needle.begin(), needle.end(), cmp) != s.end();
    }

    static std::string regexReplace(const std::string &s,
                                     const std::string &pattern,
                                     const std::string &replacement) {
        try {
            std::regex re(pattern, std::regex::icase);
            return std::regex_replace(s, re, replacement);
        } catch (...) { return s; }
    }

    // INSERT OR REPLACE INTO X(c1,c2,c3) VALUES(?,?,?)
    //   → INSERT INTO X(c1,c2,c3) VALUES(?,?,?) ON CONFLICT(c1) DO UPDATE SET c2=EXCLUDED.c2,c3=EXCLUDED.c3
    // 假设第一列是 conflict key (实际所有调用点都满足此约定)
    static std::string rewriteUpsertReplace(const std::string &sql) {
        // 匹配: INSERT OR REPLACE INTO <table>(<cols>) VALUES(...)
        std::regex re(
            R"(INSERT\s+OR\s+REPLACE\s+INTO\s+(\w+)\s*\(([^)]+)\)\s*(VALUES\s*\([^)]+\)))",
            std::regex::icase);
        std::smatch m;
        if (!std::regex_search(sql, m, re)) return sql;

        std::string table = m[1].str();
        std::string colsStr = m[2].str();
        std::string valuesPart = m[3].str();

        // 拆分 columns
        std::vector<std::string> cols;
        {
            std::string cur;
            for (char c : colsStr) {
                if (c == ',') { trim(cur); if (!cur.empty()) cols.push_back(cur); cur.clear(); }
                else cur += c;
            }
            trim(cur); if (!cur.empty()) cols.push_back(cur);
        }
        if (cols.empty()) return sql;

        std::string conflictKey = cols[0];
        std::string updateClause;
        for (size_t i = 1; i < cols.size(); ++i) {
            if (i > 1) updateClause += ",";
            updateClause += cols[i] + "=EXCLUDED." + cols[i];
        }

        std::string newSql = "INSERT INTO " + table + "(" + colsStr + ") " + valuesPart;
        if (cols.size() > 1) {
            newSql += " ON CONFLICT(" + conflictKey + ") DO UPDATE SET " + updateClause;
        } else {
            // 只有 1 列(就是冲突键): 不需要 update,做 DO NOTHING
            newSql += " ON CONFLICT(" + conflictKey + ") DO NOTHING";
        }

        // 替换原 SQL 中匹配的部分
        return std::regex_replace(sql, re, newSql, std::regex_constants::format_first_only);
    }

    static void trim(std::string &s) {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace((unsigned char)s[a])) ++a;
        while (b > a && std::isspace((unsigned char)s[b-1])) --b;
        s = s.substr(a, b - a);
    }

    // 把 ? 占位符转为 $1,$2,... 供 PG 使用
    static std::string toPgPlaceholders(const std::string &sql) {
        std::string out; out.reserve(sql.size() + 8);
        int idx = 1;
        bool inStr = false;
        char strCh = 0;
        for (size_t i = 0; i < sql.size(); ++i) {
            char c = sql[i];
            if (inStr) { out += c; if (c == strCh) inStr = false; continue; }
            if (c == '\'' || c == '"') { inStr = true; strCh = c; out += c; continue; }
            if (c == '?') { out += '$'; out += std::to_string(idx++); }
            else out += c;
        }
        return out;
    }

    // ── PG 方言转换 ─────────────────────────────────────────────
    // 把 SQLite SQL 自动转换为 PG 兼容:
    //   INSERT OR IGNORE INTO X(...) VALUES(...) → INSERT INTO X(...) VALUES(...) ON CONFLICT DO NOTHING
    //   INSERT OR REPLACE INTO X(c1,c2,...) VALUES(...) →
    //       INSERT INTO X(c1,c2,...) VALUES(...) ON CONFLICT (c1) DO UPDATE SET c2=EXCLUDED.c2,...
    //   INTEGER PRIMARY KEY AUTOINCREMENT → BIGSERIAL PRIMARY KEY
    //   AUTOINCREMENT (剩余) → 删除
    static std::string dialectifyForPg(const std::string &sqlIn) {
        std::string sql = sqlIn;

        // 1. INTEGER PRIMARY KEY AUTOINCREMENT → BIGSERIAL PRIMARY KEY
        sql = regexReplace(sql,
            R"(\bINTEGER\s+PRIMARY\s+KEY\s+AUTOINCREMENT\b)",
            "BIGSERIAL PRIMARY KEY");
        // 2. 移除剩余 AUTOINCREMENT
        sql = regexReplace(sql, R"(\s+AUTOINCREMENT\b)", "");

        // 3. INSERT OR IGNORE → ON CONFLICT DO NOTHING
        if (containsCi(sql, "INSERT OR IGNORE")) {
            sql = regexReplace(sql, R"(INSERT\s+OR\s+IGNORE\s+INTO)", "INSERT INTO");
            // 末尾追加 ON CONFLICT DO NOTHING (除非已包含)
            if (!containsCi(sql, "ON CONFLICT")) {
                // 去掉行尾分号(如有)再追加
                std::string trimmed = sql;
                while (!trimmed.empty() && (trimmed.back() == ';' || trimmed.back() == ' '))
                    trimmed.pop_back();
                sql = trimmed + " ON CONFLICT DO NOTHING";
            }
        }

        // 4. INSERT OR REPLACE → ON CONFLICT(<first_col>) DO UPDATE SET ...
        if (containsCi(sql, "INSERT OR REPLACE")) {
            sql = rewriteUpsertReplace(sql);
        }

        return sql;
    }

#ifdef WEPAY_HAS_LIBPQ
    bool execPg(const std::string &sql, const std::vector<std::string> &params) {
        std::lock_guard<std::mutex> lk(pgMtx_);
        std::string pgSql = toPgPlaceholders(dialectifyForPg(sql));
        std::vector<const char *> values; values.reserve(params.size());
        for (auto &p : params) values.push_back(p.c_str());
        PGresult *res = PQexecParams(pg_, pgSql.c_str(), (int)params.size(),
                                     nullptr, values.data(), nullptr, nullptr, 0);
        ExecStatusType st = PQresultStatus(res);
        bool ok = (st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK);
        if (!ok) {
            LOG_ERROR << "[PayDb][PG] exec: " << PQresultErrorMessage(res)
                      << " SQL=" << pgSql;
        }
        PQclear(res);
        return ok;
    }

    Rows queryPg(const std::string &sql, const std::vector<std::string> &params) {
        std::lock_guard<std::mutex> lk(pgMtx_);
        Rows out;
        std::string pgSql = toPgPlaceholders(dialectifyForPg(sql));
        std::vector<const char *> values; values.reserve(params.size());
        for (auto &p : params) values.push_back(p.c_str());
        PGresult *res = PQexecParams(pg_, pgSql.c_str(), (int)params.size(),
                                     nullptr, values.data(), nullptr, nullptr, 0);
        if (PQresultStatus(res) != PGRES_TUPLES_OK) {
            LOG_ERROR << "[PayDb][PG] query: " << PQresultErrorMessage(res)
                      << " SQL=" << sql;
            PQclear(res);
            return out;
        }
        int rows = PQntuples(res), cols = PQnfields(res);
        out.reserve(rows);
        for (int i = 0; i < rows; ++i) {
            Row r;
            for (int c = 0; c < cols; ++c) {
                const char *name = PQfname(res, c);
                const char *val = PQgetvalue(res, i, c);
                bool isNull = PQgetisnull(res, i, c);
                r[name ? name : ""] = isNull ? "" : (val ? val : "");
            }
            out.push_back(std::move(r));
        }
        PQclear(res);
        return out;
    }
#else
    bool execPg(const std::string &, const std::vector<std::string> &) { return false; }
    Rows queryPg(const std::string &, const std::vector<std::string> &) { return {}; }
#endif
};
