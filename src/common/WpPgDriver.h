// WePay-Cpp — PostgreSQL 驱动包装
// WpPgDriver.h — 自研 PG 驱动的 C++ 薄包装层
// 当 WEPAY_HAS_PG_DRIVER=1 时，PayDb 的 PG 路径经由此类调用
// 保持与原 PayDb::Row/Rows 完全兼容的接口
#pragma once // 防止头文件重复包含

#ifdef WEPAY_HAS_PG_DRIVER // 可选的 PostgreSQL 驱动支持

#include <string> // 字符串库
#include <vector>
#include <unordered_map>
#include <stdexcept>
#include <trantor/utils/Logger.h>

extern "C" {
#include "pg_config.h"
#include "pg_client.h"
#include "pg_result.h"
#include "pg_transaction.h"
#include "pg_error.h"
}

class WpPgDriver {
public:
    using Row  = std::unordered_map<std::string, std::string>;
    using Rows = std::vector<Row>;

    static WpPgDriver &instance() {
        static WpPgDriver inst;
        return inst;
    }

    // 初始化：从 PayDb::connectPg 调用，传入 libpq 连接字符串
    bool init(const std::string &connStr,
              int minPool = 3, int initPool = 8, int maxPool = 24) {
        pg_config_t *cfg = pg_config_get_instance();
        pg_config_set_connection_string(cfg, connStr.c_str());
        pg_config_set_pool_min_size(cfg,  minPool);
        pg_config_set_pool_init_size(cfg, initPool);
        pg_config_set_pool_max_size(cfg,  maxPool);
        pg_config_set_fallback_pool_min_size(cfg,  2);
        pg_config_set_fallback_pool_init_size(cfg, 2);
        pg_config_set_fallback_pool_max_size(cfg,  3);
        pg_config_set_failure_threshold(cfg,  5);
        pg_config_set_recovery_threshold(cfg, 10);
        pg_config_set_health_check_interval_sec(cfg, 30);
        pg_config_set_slow_query_threshold_ms(cfg, 100);
        pg_config_set_async_write_queue_size(cfg, 50000);
        pg_config_set_async_write_batch_size(cfg, 200);

        if (pg_client_init(connStr.c_str()) != 0) {
            LOG_ERROR << "[WpPgDriver] 初始化失败";
            return false;
        }
        pg_client_warmup_pool();
        ready_ = true;
        LOG_INFO << "[WpPgDriver] 已就绪，连接池 " << initPool << "~" << maxPool;
        return true;
    }

    void cleanup() {
        if (ready_) {
            pg_client_cleanup();
            ready_ = false;
        }
    }

    bool isReady() const { return ready_; }

    // 执行写操作（INSERT/UPDATE/DELETE）
    bool exec(const std::string &sql, const std::vector<std::string> &params) {
        auto [cparams, n] = toCParams(params);
        pg_result_t *r = pg_client_exec(sql.c_str(), cparams.data(), n);
        bool ok = r && !pg_result_is_error(r);
        if (!ok && r) {
            LOG_ERROR << "[WpPgDriver] exec failed: "
                      << (pg_result_error_message(r) ? pg_result_error_message(r) : "?")
                      << " SQL=" << sql;
        }
        if (r) pg_result_destroy(r);
        return ok;
    }

    // 执行读操作（SELECT）
    Rows query(const std::string &sql, const std::vector<std::string> &params) {
        auto [cparams, n] = toCParams(params);
        pg_result_t *r = pg_client_exec(sql.c_str(), cparams.data(), n);
        Rows out;
        if (!r || pg_result_is_error(r)) {
            if (r) {
                LOG_ERROR << "[WpPgDriver] query failed: "
                          << (pg_result_error_message(r) ? pg_result_error_message(r) : "?")
                          << " SQL=" << sql;
                pg_result_destroy(r);
            }
            return out;
        }
        int rows = pg_result_n_tuples(r);
        int cols  = pg_result_n_columns(r);
        out.reserve(rows);
        for (int i = 0; i < rows; ++i) {
            Row row;
            pg_row_t pgrow = pg_result_row(r, i);
            for (int c = 0; c < cols; ++c) {
                const char *name = pg_result_column_name(r, c);
                const char *val  = pg_row_get_value(&pgrow, c);
                if (name) row[name] = val ? val : "";
            }
            out.push_back(std::move(row));
        }
        pg_result_destroy(r);
        return out;
    }

    // 事务：func 接收一个 lambda，内部用独占连接执行
    bool transaction(const std::function<bool(pg_transaction_t*)> &func) {
        pg_transaction_t *txn = pg_transaction_create();
        if (!txn) return false;
        bool ok = false;
        if (pg_transaction_begin(txn) == PG_ERROR_SUCCESS) {
            ok = func(txn);
            if (ok) pg_transaction_commit(txn);
            else    pg_transaction_rollback(txn);
        }
        pg_transaction_destroy(txn);
        return ok;
    }

private:
    bool ready_ = false;

    // vector<string> → vector<const char*> + count
    static std::pair<std::vector<const char*>, int>
    toCParams(const std::vector<std::string> &params) {
        std::vector<const char*> cp;
        cp.reserve(params.size());
        for (auto &p : params) cp.push_back(p.c_str());
        return {std::move(cp), static_cast<int>(params.size())};
    }
};

#endif // WEPAY_HAS_PG_DRIVER
