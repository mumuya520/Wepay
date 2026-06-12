// WePay-Cpp 第三方支付系统 — 数据库初始化
// 多商户体系 + 支付通道 + 结算 + 资金日志 + 设备管理
// 路由体系:
//   /admin/api/     管理员后台
//   /merchant/api/  商户后台
//   /gateway/       统一支付网关
//   /notify/        第三方异步回调
//   /device/        设备上报
#pragma once
#include <string>
#include <vector>
#include <ctime>
#include <trantor/utils/Logger.h>
#include "PayDb.h"

class DatabaseInit {
public:
    static void run() {
        auto &db = PayDb::instance();

        if (db.isUsingSqlite()) {
            runSqlite(db);
        } else {
            initSqliteSchema(db);   // 先建 SQLite 从库表，再跑 PG 初始化（sync 才有目标表）
            runPg(db);
        }
        applyMigrations(db);
        LOG_INFO << "[DatabaseInit] 完成，后端: " << db.backendInfo();
    }

private:
    static void runSqlite(PayDb &db) {
        for (auto &sql : getSqliteCreateSqls())
            db.execSqliteDirect(sql);
        insertDefaults(db);
        LOG_INFO << "[DatabaseInit] SQLite 表初始化完成";
    }

    static void runPg(PayDb &db) {
        for (auto &sql : getPgCreateSqls())
            db.execPgOnly(sql);
        insertDefaults(db);
        LOG_INFO << "[DatabaseInit] PG 表初始化完成";
    }

    static void initSqliteSchema(PayDb &db) {
        for (auto &sql : getSqliteCreateSqls())
            db.execSqliteDirect(sql);
    }

    // ── SQLite → PG 语法自动转换 ────────────────────────────────
    static std::string sqlToPg(const std::string &sql) {
        std::string s = sql;

        // 1) INTEGER PRIMARY KEY AUTOINCREMENT → SERIAL PRIMARY KEY
        //    也处理 "id INTEGER PRIMARY KEY AUTOINCREMENT" 的情形
        {
            std::string from = "INTEGER PRIMARY KEY AUTOINCREMENT";
            std::string to   = "SERIAL PRIMARY KEY";
            auto pos = s.find(from);
            if (pos != std::string::npos) s.replace(pos, from.size(), to);
        }

        // 2) INSERT OR IGNORE → INSERT ... ON CONFLICT DO NOTHING
        //    "INSERT OR IGNORE INTO tbl(...) VALUES..." →
        //    "INSERT INTO tbl(...) VALUES... ON CONFLICT DO NOTHING"
        {
            std::string from = "INSERT OR IGNORE INTO";
            auto pos = s.find(from);
            if (pos != std::string::npos) {
                s.replace(pos, from.size(), "INSERT INTO");
                // Append ON CONFLICT DO NOTHING at end
                // Remove trailing whitespace/semicolons first
                while (!s.empty() && (s.back() == ' ' || s.back() == ';' || s.back() == '\n'))
                    s.pop_back();
                s += " ON CONFLICT DO NOTHING";
            }
        }

        // 3) CREATE INDEX IF NOT EXISTS — PG supports this, no change needed

        // 4) TEXT NOT NULL DEFAULT '' — PG supports, no change needed

        // 5) UNIQUE(...) inline constraint — PG supports, no change needed

        // 6) (datetime('now')) → NOW()  — SQLite built-in, PG uses NOW()
        {
            const std::string from = "(datetime('now'))";
            const std::string to   = "NOW()";
            size_t pos = 0;
            while ((pos = s.find(from, pos)) != std::string::npos) {
                s.replace(pos, from.size(), to);
                pos += to.size();
            }
        }

        // 7) ALTER TABLE ... ADD COLUMN → ADD COLUMN IF NOT EXISTS (PG 9.6+)
        //    让重复添加列不报错，实现幂等 migration
        {
            // 匹配: ALTER TABLE xxx ADD COLUMN yyy
            // 转换: ALTER TABLE xxx ADD COLUMN IF NOT EXISTS yyy
            const std::string pattern = "ADD COLUMN ";
            size_t pos = 0;
            while ((pos = s.find(pattern, pos)) != std::string::npos) {
                // 检查前面是否有 "ALTER TABLE"
                size_t alterPos = s.rfind("ALTER TABLE", pos);
                if (alterPos != std::string::npos && alterPos < pos) {
                    // 检查是否已经有 IF NOT EXISTS
                    size_t afterPattern = pos + pattern.size();
                    if (s.substr(afterPattern, 14) != "IF NOT EXISTS ") {
                        s.replace(pos, pattern.size(), "ADD COLUMN IF NOT EXISTS ");
                    }
                }
                pos += pattern.size();
            }
        }

        return s;
    }

    // ── 增量迁移 ───────────────────────────────────────────────
    static void applyMigrations(PayDb &db) {
        db.execSqliteDirect(
            "CREATE TABLE IF NOT EXISTS schema_version("
            "version_id INTEGER PRIMARY KEY,"
            "description TEXT NOT NULL,"
            "applied_at  TEXT NOT NULL DEFAULT (datetime('now')))");
        if (!db.isUsingSqlite())
            db.execPgOnly(
                "CREATE TABLE IF NOT EXISTS schema_version("
                "version_id BIGINT PRIMARY KEY,"
                "description TEXT NOT NULL,"
                "applied_at  TIMESTAMP NOT NULL DEFAULT NOW())");

        struct Migration { int ver; const char *desc; const char *sql; };
        static const Migration MIGS[] = {
            // v1~v5: 旧迁移保留兼容
            {1, "add name column to pay_order",
             "ALTER TABLE pay_order ADD COLUMN name TEXT DEFAULT ''"},
            {2, "index pay_order state+created_at",
             "CREATE INDEX IF NOT EXISTS idx_order_state_date ON pay_order(state, created_at)"},
            {3, "index tmp_price oid",
             "CREATE INDEX IF NOT EXISTS idx_tmp_price_oid ON tmp_price(oid)"},
            {4, "add account column to pay_qrcode",
             "ALTER TABLE pay_qrcode ADD COLUMN account TEXT NOT NULL DEFAULT ''"},
            {5, "add pattern column to pay_qrcode",
             "ALTER TABLE pay_qrcode ADD COLUMN pattern INTEGER NOT NULL DEFAULT 1"},
            // v10+: 多商户改造
            {10, "index pay_order mch_id",
             "CREATE INDEX IF NOT EXISTS idx_order_mch ON pay_order(mch_id)"},
            {11, "index pay_order channel_id",
             "CREATE INDEX IF NOT EXISTS idx_order_channel ON pay_order(channel_id)"},
            {12, "index settle_order mch_id+state",
             "CREATE INDEX IF NOT EXISTS idx_settle_mch_state ON settle_order(mch_id, state)"},
            {13, "index money_log mch_id",
             "CREATE INDEX IF NOT EXISTS idx_money_log_mch ON money_log(mch_id)"},
            {14, "index device mch_id+state",
             "CREATE INDEX IF NOT EXISTS idx_device_mch ON device(mch_id, state)"},

            // ══════════════════════════════════════════════════════
            // Phase 1 业务核心：RBAC / 代理商 / 分账 / 转账 / JWT 刷新
            // ══════════════════════════════════════════════════════

            // v20~v24: RBAC 权限系统
            {20, "create sys_user",
             "CREATE TABLE IF NOT EXISTS sys_user("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "username TEXT NOT NULL UNIQUE,"
             "password TEXT NOT NULL,"
             "salt TEXT NOT NULL DEFAULT '',"
             "real_name TEXT NOT NULL DEFAULT '',"
             "phone TEXT NOT NULL DEFAULT '',"
             "email TEXT NOT NULL DEFAULT '',"
             "avatar TEXT NOT NULL DEFAULT '',"
             "is_super INTEGER NOT NULL DEFAULT 0,"
             "state INTEGER NOT NULL DEFAULT 1,"
             "last_login_ip TEXT NOT NULL DEFAULT '',"
             "last_login_at INTEGER NOT NULL DEFAULT 0,"
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "updated_at INTEGER NOT NULL DEFAULT 0)"},
            {21, "create sys_role",
             "CREATE TABLE IF NOT EXISTS sys_role("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "role_code TEXT NOT NULL UNIQUE,"
             "role_name TEXT NOT NULL,"
             "remark TEXT NOT NULL DEFAULT '',"
             "state INTEGER NOT NULL DEFAULT 1,"
             "created_at INTEGER NOT NULL DEFAULT 0)"},
            {22, "create sys_permission",
             "CREATE TABLE IF NOT EXISTS sys_permission("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "perm_code TEXT NOT NULL UNIQUE,"
             "perm_name TEXT NOT NULL,"
             "module TEXT NOT NULL DEFAULT '',"
             "created_at INTEGER NOT NULL DEFAULT 0)"},
            {23, "create sys_user_role",
             "CREATE TABLE IF NOT EXISTS sys_user_role("
             "user_id INTEGER NOT NULL,"
             "role_id INTEGER NOT NULL,"
             "PRIMARY KEY(user_id, role_id))"},
            {24, "create sys_role_permission",
             "CREATE TABLE IF NOT EXISTS sys_role_permission("
             "role_id INTEGER NOT NULL,"
             "perm_id INTEGER NOT NULL,"
             "PRIMARY KEY(role_id, perm_id))"},

            // v30~v33: 代理商
            {30, "create agent",
             "CREATE TABLE IF NOT EXISTS agent("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "agent_no TEXT NOT NULL UNIQUE,"
             "username TEXT NOT NULL UNIQUE,"
             "password TEXT NOT NULL,"
             "salt TEXT NOT NULL DEFAULT '',"
             "agent_name TEXT NOT NULL DEFAULT '',"
             "contact TEXT NOT NULL DEFAULT '',"
             "phone TEXT NOT NULL DEFAULT '',"
             "email TEXT NOT NULL DEFAULT '',"
             "parent_id INTEGER NOT NULL DEFAULT 0,"
             "level INTEGER NOT NULL DEFAULT 1,"
             "commission_rate TEXT NOT NULL DEFAULT '0.10',"
             "balance TEXT NOT NULL DEFAULT '0.00',"
             "total_commission TEXT NOT NULL DEFAULT '0.00',"
             "state INTEGER NOT NULL DEFAULT 1,"
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "updated_at INTEGER NOT NULL DEFAULT 0)"},
            {31, "merchant add agent_id",
             "ALTER TABLE merchant ADD COLUMN agent_id INTEGER NOT NULL DEFAULT 0"},
            {32, "index merchant agent_id",
             "CREATE INDEX IF NOT EXISTS idx_merchant_agent ON merchant(agent_id)"},
            {33, "create agent_commission_log",
             "CREATE TABLE IF NOT EXISTS agent_commission_log("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "agent_id INTEGER NOT NULL,"
             "order_id TEXT NOT NULL,"
             "mch_id INTEGER NOT NULL,"
             "order_amount TEXT NOT NULL DEFAULT '0.00',"
             "commission TEXT NOT NULL DEFAULT '0.00',"
             "rate TEXT NOT NULL DEFAULT '0.00',"
             "state INTEGER NOT NULL DEFAULT 0,"
             "created_at INTEGER NOT NULL DEFAULT 0)"},

            // v40~v42: 分账
            {40, "create division_receiver",
             "CREATE TABLE IF NOT EXISTS division_receiver("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "mch_id INTEGER NOT NULL,"
             "receiver_code TEXT NOT NULL,"
             "receiver_name TEXT NOT NULL,"
             "account_type INTEGER NOT NULL DEFAULT 1,"   // 1=个人 2=商户 3=代理
             "account_no TEXT NOT NULL DEFAULT '',"
             "bind_ratio TEXT NOT NULL DEFAULT '0.00',"    // 默认分账比例
             "state INTEGER NOT NULL DEFAULT 1,"
             "created_at INTEGER NOT NULL DEFAULT 0)"},
            {41, "create division_record",
             "CREATE TABLE IF NOT EXISTS division_record("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "order_id TEXT NOT NULL,"
             "mch_id INTEGER NOT NULL,"
             "receiver_id INTEGER NOT NULL,"
             "receiver_name TEXT NOT NULL DEFAULT '',"
             "order_amount TEXT NOT NULL DEFAULT '0.00',"
             "ratio TEXT NOT NULL DEFAULT '0.00',"
             "division_amount TEXT NOT NULL DEFAULT '0.00',"
             "state INTEGER NOT NULL DEFAULT 0,"           // 0=待处理 1=成功 -1=失败
             "err_msg TEXT NOT NULL DEFAULT '',"
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "finished_at INTEGER NOT NULL DEFAULT 0)"},
            {42, "index division_record",
             "CREATE INDEX IF NOT EXISTS idx_division_order ON division_record(order_id)"},

            // v50~v51: 转账
            {50, "create transfer_order",
             "CREATE TABLE IF NOT EXISTS transfer_order("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "transfer_no TEXT NOT NULL UNIQUE,"
             "mch_id INTEGER NOT NULL,"
             "mch_transfer_no TEXT NOT NULL DEFAULT '',"  // 商户转账单号
             "channel_id INTEGER NOT NULL DEFAULT 0,"
             "amount TEXT NOT NULL DEFAULT '0.00',"
             "fee TEXT NOT NULL DEFAULT '0.00',"
             "real_amount TEXT NOT NULL DEFAULT '0.00',"
             "account_type INTEGER NOT NULL DEFAULT 1,"   // 1=微信openid 2=支付宝账号 3=银行卡
             "account_no TEXT NOT NULL,"
             "account_name TEXT NOT NULL DEFAULT '',"
             "remark TEXT NOT NULL DEFAULT '',"
             "channel_transfer_no TEXT NOT NULL DEFAULT '',"
             "state INTEGER NOT NULL DEFAULT 0,"           // 0=处理中 1=成功 -1=失败
             "err_msg TEXT NOT NULL DEFAULT '',"
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "finished_at INTEGER NOT NULL DEFAULT 0)"},
            {51, "index transfer_order mch",
             "CREATE INDEX IF NOT EXISTS idx_transfer_mch ON transfer_order(mch_id, state)"},

            // v60~v61: JWT 刷新令牌 + 黑名单
            {60, "create jwt_refresh_token",
             "CREATE TABLE IF NOT EXISTS jwt_refresh_token("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "refresh_token TEXT NOT NULL UNIQUE,"
             "user_type INTEGER NOT NULL,"                 // 1=sys_user 2=merchant 3=agent
             "user_id INTEGER NOT NULL,"
             "expires_at INTEGER NOT NULL,"
             "revoked INTEGER NOT NULL DEFAULT 0,"
             "created_at INTEGER NOT NULL DEFAULT 0)"},
            {61, "create jwt_blacklist",
             "CREATE TABLE IF NOT EXISTS jwt_blacklist("
             "token_hash TEXT PRIMARY KEY,"                 // token 的 sha256
             "expires_at INTEGER NOT NULL,"
             "revoked_at INTEGER NOT NULL DEFAULT 0)"},
            {62, "index jwt_refresh user",
             "CREATE INDEX IF NOT EXISTS idx_jwt_refresh_user ON jwt_refresh_token(user_type, user_id)"},

            // ══════════════════════════════════════════════════════
            // Phase 4 扩展：配置细化 / 多层级 / 实用工具
            // ══════════════════════════════════════════════════════

            // v70~v74: ISV 服务商层
            {70, "create isv",
             "CREATE TABLE IF NOT EXISTS isv("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "isv_no TEXT NOT NULL UNIQUE,"
             "username TEXT NOT NULL UNIQUE,"
             "password TEXT NOT NULL,"
             "salt TEXT NOT NULL DEFAULT '',"
             "isv_name TEXT NOT NULL DEFAULT '',"
             "contact TEXT NOT NULL DEFAULT '',"
             "phone TEXT NOT NULL DEFAULT '',"
             "email TEXT NOT NULL DEFAULT '',"
             "remark TEXT NOT NULL DEFAULT '',"
             "state INTEGER NOT NULL DEFAULT 1,"
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "updated_at INTEGER NOT NULL DEFAULT 0)"},
            {71, "merchant add isv_id",
             "ALTER TABLE merchant ADD COLUMN isv_id INTEGER NOT NULL DEFAULT 0"},
            {72, "agent add isv_id",
             "ALTER TABLE agent ADD COLUMN isv_id INTEGER NOT NULL DEFAULT 0"},
            {73, "index merchant isv",
             "CREATE INDEX IF NOT EXISTS idx_merchant_isv ON merchant(isv_id)"},
            {74, "index agent isv",
             "CREATE INDEX IF NOT EXISTS idx_agent_isv ON agent(isv_id)"},

            // v75~v78: 商户应用 / 门店
            {75, "create mch_app (重建规范)",
             "CREATE TABLE IF NOT EXISTS mch_app_v2("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "mch_id INTEGER NOT NULL,"
             "app_id TEXT NOT NULL UNIQUE,"
             "app_name TEXT NOT NULL DEFAULT '',"
             "app_secret TEXT NOT NULL DEFAULT '',"
             "sign_type TEXT NOT NULL DEFAULT 'MD5',"
             "ip_white TEXT NOT NULL DEFAULT '',"
             "notify_url TEXT NOT NULL DEFAULT '',"
             "return_url TEXT NOT NULL DEFAULT '',"
             "remark TEXT NOT NULL DEFAULT '',"
             "state INTEGER NOT NULL DEFAULT 1,"
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "updated_at INTEGER NOT NULL DEFAULT 0)"},
            {76, "create mch_store",
             "CREATE TABLE IF NOT EXISTS mch_store("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "mch_id INTEGER NOT NULL,"
             "app_id TEXT NOT NULL DEFAULT '',"
             "store_name TEXT NOT NULL,"
             "store_addr TEXT NOT NULL DEFAULT '',"
             "contact TEXT NOT NULL DEFAULT '',"
             "phone TEXT NOT NULL DEFAULT '',"
             "remark TEXT NOT NULL DEFAULT '',"
             "state INTEGER NOT NULL DEFAULT 1,"
             "created_at INTEGER NOT NULL DEFAULT 0)"},
            {77, "index mch_app mch_id",
             "CREATE INDEX IF NOT EXISTS idx_mch_app_v2_mch ON mch_app_v2(mch_id)"},
            {78, "index mch_store mch_id",
             "CREATE INDEX IF NOT EXISTS idx_mch_store_mch ON mch_store(mch_id)"},

            // v80~v84: 精细化配置
            {80, "create pay_interface_define",  // 支付接口定义(如 wxpay_v3 / alipay_v1)
             "CREATE TABLE IF NOT EXISTS pay_interface_define("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "if_code TEXT NOT NULL UNIQUE,"        // wxpay / alipay / unionpay
             "if_name TEXT NOT NULL,"
             "is_mch_mode INTEGER NOT NULL DEFAULT 1,"  // 1=支持普通商户
             "is_isv_mode INTEGER NOT NULL DEFAULT 0,"  // 1=支持服务商模式
             "way_codes TEXT NOT NULL DEFAULT '',"      // 支持的支付方式(逗号分隔)
             "config_params TEXT NOT NULL DEFAULT '[]',"// 配置字段JSON(前端渲染表单)
             "icon TEXT NOT NULL DEFAULT '',"
             "state INTEGER NOT NULL DEFAULT 1,"
             "remark TEXT NOT NULL DEFAULT '')"},
            {81, "create pay_interface_config", // 商户/ISV的具体接口参数实例
             "CREATE TABLE IF NOT EXISTS pay_interface_config("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "info_type INTEGER NOT NULL,"        // 1=ISV 2=商户 3=商户App
             "info_id TEXT NOT NULL,"             // 关联 ISV.isv_no/merchant.mch_no/mch_app.app_id
             "if_code TEXT NOT NULL,"             // 接口code，关联 pay_interface_define
             "if_params TEXT NOT NULL DEFAULT '{}',"  // 实际配置值JSON
             "if_rate TEXT NOT NULL DEFAULT '0.60',"  // 该接口费率
             "state INTEGER NOT NULL DEFAULT 1,"
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "updated_at INTEGER NOT NULL DEFAULT 0)"},
            {82, "idx pay_interface_config",
             "CREATE INDEX IF NOT EXISTS idx_pif_config ON pay_interface_config(info_type,info_id,if_code)"},
            {83, "create pay_rate_config",  // 细粒度费率(按商户+支付方式)
             "CREATE TABLE IF NOT EXISTS pay_rate_config("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "config_mode INTEGER NOT NULL,"     // 1=ISV 2=商户 3=应用
             "info_id TEXT NOT NULL,"
             "way_code TEXT NOT NULL,"            // wx_native / ali_bar / ysf_jsapi...
             "rate TEXT NOT NULL DEFAULT '0.60',"
             "state INTEGER NOT NULL DEFAULT 1,"
             "updated_at INTEGER NOT NULL DEFAULT 0)"},
            {84, "create pay_oauth2_config",  // OAuth2 授权(获取openid)
             "CREATE TABLE IF NOT EXISTS pay_oauth2_config("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "mch_id INTEGER NOT NULL,"
             "app_id TEXT NOT NULL DEFAULT '',"
             "oauth_type TEXT NOT NULL,"         // wx_jsapi / ali_jsapi
             "oauth_app_id TEXT NOT NULL DEFAULT '',"
             "oauth_secret TEXT NOT NULL DEFAULT '',"
             "redirect_url TEXT NOT NULL DEFAULT '',"
             "state INTEGER NOT NULL DEFAULT 1)"},

            // v90~v92: 分账接收组 / 商户端子账号权限
            {90, "create division_receiver_group",
             "CREATE TABLE IF NOT EXISTS division_receiver_group("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "mch_id INTEGER NOT NULL,"
             "group_name TEXT NOT NULL,"
             "auto_div_flag INTEGER NOT NULL DEFAULT 0,"  // 1=自动分账
             "state INTEGER NOT NULL DEFAULT 1,"
             "created_at INTEGER NOT NULL DEFAULT 0)"},
            {91, "division_receiver add group_id",
             "ALTER TABLE division_receiver ADD COLUMN group_id INTEGER NOT NULL DEFAULT 0"},
            {92, "create mch_user",  // 商户端子账号
             "CREATE TABLE IF NOT EXISTS mch_user("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "mch_id INTEGER NOT NULL,"
             "username TEXT NOT NULL,"
             "password TEXT NOT NULL,"
             "salt TEXT NOT NULL DEFAULT '',"
             "real_name TEXT NOT NULL DEFAULT '',"
             "phone TEXT NOT NULL DEFAULT '',"
             "email TEXT NOT NULL DEFAULT '',"
             "is_admin INTEGER NOT NULL DEFAULT 0,"
             "permissions TEXT NOT NULL DEFAULT '[]',"   // 权限code数组JSON
             "state INTEGER NOT NULL DEFAULT 1,"
             "last_login_at INTEGER NOT NULL DEFAULT 0,"
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "UNIQUE(mch_id, username))"},

            // v95~v99: 实用功能
            {95, "create sys_article",  // 系统公告
             "CREATE TABLE IF NOT EXISTS sys_article("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "title TEXT NOT NULL,"
             "content TEXT NOT NULL DEFAULT '',"
             "article_type INTEGER NOT NULL DEFAULT 1,"  // 1=公告 2=帮助 3=更新日志
             "target TEXT NOT NULL DEFAULT 'all',"       // all / merchant / agent / isv
             "is_top INTEGER NOT NULL DEFAULT 0,"
             "publisher TEXT NOT NULL DEFAULT '',"
             "state INTEGER NOT NULL DEFAULT 1,"
             "publish_at INTEGER NOT NULL DEFAULT 0,"
             "created_at INTEGER NOT NULL DEFAULT 0)"},
            {96, "create account_bill",  // 对账单
             "CREATE TABLE IF NOT EXISTS account_bill("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "bill_date TEXT NOT NULL,"                  // 2026-05 / 2026-05-03
             "bill_type INTEGER NOT NULL DEFAULT 1,"     // 1=日账单 2=月账单
             "mch_id INTEGER NOT NULL DEFAULT 0,"        // 0=全局
             "order_count INTEGER NOT NULL DEFAULT 0,"
             "total_amount TEXT NOT NULL DEFAULT '0.00',"
             "refund_count INTEGER NOT NULL DEFAULT 0,"
             "refund_amount TEXT NOT NULL DEFAULT '0.00',"
             "fee_amount TEXT NOT NULL DEFAULT '0.00',"
             "net_amount TEXT NOT NULL DEFAULT '0.00',"
             "file_path TEXT NOT NULL DEFAULT '',"
             "generated_at INTEGER NOT NULL DEFAULT 0,"
             "UNIQUE(bill_date, bill_type, mch_id))"},
            {97, "index account_bill mch",
             "CREATE INDEX IF NOT EXISTS idx_bill_mch ON account_bill(mch_id)"},
            {98, "create sys_menu",  // 菜单树(前端渲染左侧导航)
             "CREATE TABLE IF NOT EXISTS sys_menu("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "parent_id INTEGER NOT NULL DEFAULT 0,"
             "menu_name TEXT NOT NULL,"
             "menu_path TEXT NOT NULL DEFAULT '',"
             "menu_icon TEXT NOT NULL DEFAULT '',"
             "perm_code TEXT NOT NULL DEFAULT '',"     // 关联 sys_permission
             "sort_order INTEGER NOT NULL DEFAULT 0,"
             "target TEXT NOT NULL DEFAULT 'admin',"   // admin/merchant/agent/isv
             "state INTEGER NOT NULL DEFAULT 1)"},
            {99, "default pay_interface_define",
             "INSERT OR IGNORE INTO pay_interface_define(if_code,if_name,is_mch_mode,way_codes,state) "
             "VALUES"
             "('wxpay','微信官方支付',1,'wx_native,wx_jsapi,wx_bar,wx_h5,wx_app',1),"
             "('alipay','支付宝官方支付',1,'ali_qr,ali_bar,ali_jsapi,ali_wap',1),"
             "('unionpay','云闪付',1,'ysf_qr,ysf_bar,ysf_jsapi',1),"
             "('epay','易支付上游',1,'wxpay,alipay,qqpay',1),"
             "('monitor','免签监听',1,'wxpay,alipay,qqpay',1)"},

            // v100+: 退款单/订单字段补全 (Phase5A)
            {100, "refund_order add err_msg",
             "ALTER TABLE refund_order ADD COLUMN err_msg TEXT NOT NULL DEFAULT ''"},
            {101, "refund_order add finished_at",
             "ALTER TABLE refund_order ADD COLUMN finished_at INTEGER NOT NULL DEFAULT 0"},
            {102, "pay_order add channel_order_no",
             "ALTER TABLE pay_order ADD COLUMN channel_order_no TEXT NOT NULL DEFAULT ''"},

            // v110~v119: Phase5B 表单驱动与聚合二维码
            {110, "create pay_way",  // 支付方式(way_code)独立表，替代 pay_type 简版
             "CREATE TABLE IF NOT EXISTS pay_way("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "way_code TEXT NOT NULL UNIQUE,"           // wx_native / ali_bar / ysf_jsapi
             "way_name TEXT NOT NULL,"
             "icon TEXT NOT NULL DEFAULT '',"
             "bg_color TEXT NOT NULL DEFAULT '',"
             "if_code TEXT NOT NULL DEFAULT '',"        // 关联 pay_interface_define.if_code
             "sort_order INTEGER NOT NULL DEFAULT 0,"
             "state INTEGER NOT NULL DEFAULT 1,"
             "remark TEXT NOT NULL DEFAULT '',"
             "updated_at INTEGER NOT NULL DEFAULT 0)"},
            {111, "default pay_way",
             "INSERT OR IGNORE INTO pay_way(way_code,way_name,if_code,sort_order,state) VALUES"
             "('wx_native','微信Native扫码','wxpay',10,1),"
             "('wx_jsapi','微信JSAPI公众号','wxpay',20,1),"
             "('wx_bar','微信付款码','wxpay',30,1),"
             "('wx_h5','微信H5','wxpay',40,1),"
             "('wx_app','微信APP','wxpay',50,1),"
             "('ali_qr','支付宝扫码','alipay',110,1),"
             "('ali_bar','支付宝付款码','alipay',120,1),"
             "('ali_jsapi','支付宝生活号','alipay',130,1),"
             "('ali_wap','支付宝H5','alipay',140,1),"
             "('ysf_qr','云闪付扫码','unionpay',210,1),"
             "('ysf_bar','云闪付付款码','unionpay',220,1),"
             "('ysf_jsapi','云闪付JSAPI','unionpay',230,1)"},
            {112, "create agg_qrcode",  // 聚合二维码(永久码，扫码选支付方式)
             "CREATE TABLE IF NOT EXISTS agg_qrcode("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "qr_code TEXT NOT NULL UNIQUE,"            // 唯一码，URL: /qr/{qr_code}
             "mch_id INTEGER NOT NULL,"
             "app_id TEXT NOT NULL DEFAULT '',"
             "store_id INTEGER NOT NULL DEFAULT 0,"
             "qr_name TEXT NOT NULL DEFAULT '',"
             "qr_type INTEGER NOT NULL DEFAULT 1,"      // 1=固定金额 2=动态金额
             "fixed_amount TEXT NOT NULL DEFAULT '0.00',"
             "subject TEXT NOT NULL DEFAULT '',"
             "body TEXT NOT NULL DEFAULT '',"
             "way_codes TEXT NOT NULL DEFAULT '',"      // 允许支付方式(逗号分隔)
             "shell_id INTEGER NOT NULL DEFAULT 0,"     // 关联模板
             "total_count INTEGER NOT NULL DEFAULT 0,"  // 累计使用次数
             "total_amount TEXT NOT NULL DEFAULT '0.00',"
             "state INTEGER NOT NULL DEFAULT 1,"
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "updated_at INTEGER NOT NULL DEFAULT 0)"},
            {113, "idx agg_qrcode mch",
             "CREATE INDEX IF NOT EXISTS idx_agg_qrcode_mch ON agg_qrcode(mch_id, state)"},
            {114, "create qrcode_shell",  // 二维码模板(封面图+Logo)
             "CREATE TABLE IF NOT EXISTS qrcode_shell("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "shell_name TEXT NOT NULL,"
             "bg_image TEXT NOT NULL DEFAULT '',"
             "logo_image TEXT NOT NULL DEFAULT '',"
             "qr_size INTEGER NOT NULL DEFAULT 300,"
             "qr_x INTEGER NOT NULL DEFAULT 0,"
             "qr_y INTEGER NOT NULL DEFAULT 0,"
             "state INTEGER NOT NULL DEFAULT 1,"
             "created_at INTEGER NOT NULL DEFAULT 0)"},
            {115, "create mch_config",  // 商户扩展KV配置
             "CREATE TABLE IF NOT EXISTS mch_config("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "mch_id INTEGER NOT NULL,"
             "config_key TEXT NOT NULL,"
             "config_value TEXT NOT NULL DEFAULT '',"
             "remark TEXT NOT NULL DEFAULT '',"
             "updated_at INTEGER NOT NULL DEFAULT 0,"
             "UNIQUE(mch_id, config_key))"},
            {116, "idx mch_config",
             "CREATE INDEX IF NOT EXISTS idx_mch_config ON mch_config(mch_id)"},
            {117, "default sys_menu",  // 默认菜单树
             "INSERT OR IGNORE INTO sys_menu(id,parent_id,menu_name,menu_path,menu_icon,perm_code,sort_order,target,state) VALUES"
             "(1,0,'数据总览','/dashboard','DataAnalysis','dashboard:view',10,'admin',1),"
             "(2,0,'商户管理','/merchant','OfficeBuilding','merchant:view',20,'admin',1),"
             "(21,2,'商户列表','/merchant/list','','merchant:view',21,'admin',1),"
             "(22,2,'商户应用','/merchant/app','','merchant:view',22,'admin',1),"
             "(23,2,'商户门店','/merchant/store','','merchant:view',23,'admin',1),"
             "(3,0,'通道管理','/channel','Connection','channel:view',30,'admin',1),"
             "(4,0,'订单管理','/order','Document','order:view',40,'admin',1),"
             "(5,0,'资金管理','/finance','Money','settle:view',50,'admin',1),"
             "(6,0,'合作伙伴','/partner','Share','agent:view',60,'admin',1),"
             "(7,0,'系统管理','/system','Setting','sysuser:manage',90,'admin',1),"
             "(8,0,'插件市场','/plugin','Box','channel:edit',35,'admin',1)"},

            // v120+: 插件市场(Plugin Store)
            {120, "create plugin_store",
             "CREATE TABLE IF NOT EXISTS plugin_store("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "plugin_code TEXT NOT NULL UNIQUE,"       // 对应 ChannelPlugin::name()
             "display_name TEXT NOT NULL,"
             "description TEXT NOT NULL DEFAULT '',"
             "vendor TEXT NOT NULL DEFAULT '',"
             "version TEXT NOT NULL DEFAULT '1.0.0',"
             "icon TEXT NOT NULL DEFAULT '',"           // 图标URL或emoji
             "plugin_type INTEGER NOT NULL DEFAULT 1,"  // 1=内置(编译) 2=外置(.dll)
             "supported_ways TEXT NOT NULL DEFAULT '',"// 支持的支付方式(逗号分隔)
             "params_schema TEXT NOT NULL DEFAULT '[]',"// 配置字段JSON schema(前端渲染表单)
             "doc_url TEXT NOT NULL DEFAULT '',"        // 文档链接
             "price INTEGER NOT NULL DEFAULT 0,"        // 0=免费 >0=付费(单位:分,为未来预留)
             "installed INTEGER NOT NULL DEFAULT 0,"    // 0=未安装 1=已安装
             "state INTEGER NOT NULL DEFAULT 1,"        // 0=下架 1=可用
             "lib_path TEXT NOT NULL DEFAULT '',"       // 外置.dll路径(仅plugin_type=2)
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "installed_at INTEGER NOT NULL DEFAULT 0)"},
            {121, "default plugin_store",
             "INSERT OR IGNORE INTO plugin_store(plugin_code,display_name,description,vendor,"
             "version,icon,plugin_type,supported_ways,price,installed,state,created_at) VALUES"
             "('wxpay_native','微信支付-Native扫码','微信官方扫码支付，支持PC网页/桌面应用','腾讯','1.0.0','💚',1,'wx_native',0,1,1,0),"
             "('wxpay_ext','微信支付-扩展','付款码/JSAPI/H5/APP 多场景扩展','腾讯','1.0.0','💚',1,'wx_bar,wx_jsapi,wx_h5,wx_app',0,1,1,0),"
             "('alipay','支付宝-当面付','支付宝官方扫码支付','阿里','1.0.0','🔵',1,'ali_qr',0,1,1,0),"
             "('alipay_ext','支付宝-扩展','付款码/JSAPI/WAP/PC多场景','阿里','1.0.0','🔵',1,'ali_bar,ali_jsapi,ali_wap,ali_page',0,1,1,0),"
             "('unionpay','云闪付','银联标准接入 5.1.0 协议','银联','1.0.0','🏦',1,'ysf_qr,ysf_bar,ysf_jsapi',0,1,1,0),"
             "('epay_upstream','易支付上游','对接彩虹易/独角兽等易支付协议平台','开源社区','1.0.0','🌈',1,'wxpay,alipay,qqpay',0,1,1,0),"
             "('monitor','免签监听','通过设备监听免签收款(无官方资质可用)','开源社区','1.0.0','📱',1,'wxpay,alipay,qqpay',0,1,1,0),"
             "('allinpay','通联支付','通联全渠道聚合支付','通联','1.0.0','🔶',1,'wx_qr,wx_bar,ali_qr,ali_bar,ysf_qr',0,0,1,0),"
             "('dgpay','斗拱支付','汇付天下斗拱开放平台','汇付','1.0.0','⚡',1,'wx_qr,ali_qr,ysf_qr,wx_jsapi',0,0,1,0),"
             "('lklpay','拉卡拉','拉卡拉支付开放平台','拉卡拉','1.0.0','💳',1,'wx_qr,ali_qr,ysf_qr',0,0,1,0)"},
            {122, "default plugin_store ext (9 vendors)",
             "INSERT OR IGNORE INTO plugin_store(plugin_code,display_name,description,vendor,"
             "version,icon,plugin_type,supported_ways,price,installed,state,created_at) VALUES"
             "('hkrtpay','海科融通','海科融通聚合支付(骨架，需配置真实参数)','海科融通','1.0.0','🟢',1,'wx_qr,ali_qr,ysf_qr',0,0,1,0),"
             "('jlpay','嘉联支付','嘉联立刷支付','嘉联支付','1.0.0','🟡',1,'wx_qr,ali_qr,wx_jsapi',0,0,1,0),"
             "('lcswpay','利楚扫呗','利楚扫呗聚合扫码','利楚商服','1.0.0','🟣',1,'wx_qr,ali_qr,ysf_qr',0,0,1,0),"
             "('lespay','乐刷','乐刷支付聚合','乐刷','1.0.0','🟠',1,'wx_qr,ali_qr',0,0,1,0),"
             "('pppay','朋朋支付','朋朋支付聚合','朋朋','1.0.0','🩷',1,'wx_qr,ali_qr',0,0,1,0),"
             "('sxfpay','随行付','随行付聚合扫码','随行付','1.0.0','🟤',1,'wx_qr,ali_qr,ysf_qr',0,0,1,0),"
             "('umspay','银联商务','银联商务全民付/统一收单','银联商务','1.0.0','🔷',1,'wx_qr,ali_qr,ysf_qr,wx_jsapi',0,0,1,0),"
             "('xxpay','汇付小付','汇付小付/三方对接','汇付','1.0.0','⚪',1,'wx_qr,ali_qr',0,0,1,0),"
             "('ysepay','银盛支付','银盛通联聚合扫码','银盛','1.0.0','🟦',1,'wx_qr,ali_qr,ysf_qr',0,0,1,0)"},

            // v130+: Phase5D 安全与周边
            {130, "create login_attempt",  // 登录尝试日志(防爆破)
             "CREATE TABLE IF NOT EXISTS login_attempt("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "username TEXT NOT NULL,"
             "ip TEXT NOT NULL DEFAULT '',"
             "user_agent TEXT NOT NULL DEFAULT '',"
             "success INTEGER NOT NULL DEFAULT 0,"
             "fail_reason TEXT NOT NULL DEFAULT '',"
             "user_type INTEGER NOT NULL DEFAULT 1,"   // 1=admin 2=mch 3=agent
             "created_at INTEGER NOT NULL DEFAULT 0)"},
            {131, "idx login_attempt user",
             "CREATE INDEX IF NOT EXISTS idx_login_attempt_user ON login_attempt(username, created_at)"},
            {132, "idx login_attempt ip",
             "CREATE INDEX IF NOT EXISTS idx_login_attempt_ip ON login_attempt(ip, created_at)"},

            // v140+: Phase6A 用户团队/部门
            {140, "create sys_user_team",
             "CREATE TABLE IF NOT EXISTS sys_user_team("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "team_no TEXT NOT NULL UNIQUE,"
             "team_name TEXT NOT NULL,"
             "leader_user_id INTEGER NOT NULL DEFAULT 0,"
             "owner_type INTEGER NOT NULL DEFAULT 1,"  // 1=admin 2=isv 3=agent 4=mch
             "owner_id TEXT NOT NULL DEFAULT '',"
             "remark TEXT NOT NULL DEFAULT '',"
             "state INTEGER NOT NULL DEFAULT 1,"
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "updated_at INTEGER NOT NULL DEFAULT 0)"},
            {141, "sys_user add team_id",
             "ALTER TABLE sys_user ADD COLUMN team_id INTEGER NOT NULL DEFAULT 0"},
            {142, "idx sys_user_team owner",
             "CREATE INDEX IF NOT EXISTS idx_sys_user_team_owner ON sys_user_team(owner_type, owner_id)"},

            // v150~v157: 旧 V免签 pay_order 列名兼容迁移 (SQLite 3.25+)
            // 如果旧表存在这些列，重命名为新列名；新表已用新名，会失败但不影响
            {150, "rename pay_order.create_date to created_at",
             "ALTER TABLE pay_order RENAME COLUMN create_date TO created_at"},
            {151, "rename pay_order.really_price to real_amount",
             "ALTER TABLE pay_order RENAME COLUMN really_price TO real_amount"},
            {152, "rename pay_order.close_date to expire_time",
             "ALTER TABLE pay_order RENAME COLUMN close_date TO expire_time"},
            {153, "rename pay_order.pay_date to pay_time",
             "ALTER TABLE pay_order RENAME COLUMN pay_date TO pay_time"},
            {154, "rename pay_order.price to amount",
             "ALTER TABLE pay_order RENAME COLUMN price TO amount"},
            {155, "rename pay_order.pay_id to mch_order_no",
             "ALTER TABLE pay_order RENAME COLUMN pay_id TO mch_order_no"},
            {156, "rename pay_order.type to pay_type",
             "ALTER TABLE pay_order RENAME COLUMN type TO pay_type"},
            {157, "rename pay_order.name to subject",
             "ALTER TABLE pay_order RENAME COLUMN name TO subject"},

            {200, "source compat merchant domain",
             "CREATE TABLE IF NOT EXISTS merchant_domain("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "mch_id INTEGER NOT NULL,"
             "site_name TEXT NOT NULL DEFAULT '',"
             "site_url TEXT NOT NULL,"
             "state INTEGER NOT NULL DEFAULT 0,"
             "remark TEXT NOT NULL DEFAULT '',"
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "updated_at INTEGER NOT NULL DEFAULT 0)"},
            {201, "source compat ticket",
             "CREATE TABLE IF NOT EXISTS support_ticket("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "mch_id INTEGER NOT NULL,"
             "category TEXT NOT NULL DEFAULT '',"
             "title TEXT NOT NULL,"
             "content TEXT NOT NULL DEFAULT '',"
             "reply_content TEXT NOT NULL DEFAULT '',"
             "state INTEGER NOT NULL DEFAULT 0,"
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "reply_at INTEGER NOT NULL DEFAULT 0)"},
            {202, "source compat cdk",
             "CREATE TABLE IF NOT EXISTS cdk_code("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "code TEXT NOT NULL UNIQUE,"
             "cdk_type INTEGER NOT NULL DEFAULT 1,"
             "value TEXT NOT NULL DEFAULT '0',"
             "state INTEGER NOT NULL DEFAULT 0,"
             "used_mch_id INTEGER NOT NULL DEFAULT 0,"
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "used_at INTEGER NOT NULL DEFAULT 0)"},
            {203, "source compat vip package",
             "CREATE TABLE IF NOT EXISTS vip_package("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "name TEXT NOT NULL,"
             "price TEXT NOT NULL DEFAULT '0.00',"
             "days INTEGER NOT NULL DEFAULT 30,"
             "rate TEXT NOT NULL DEFAULT '0.00',"
             "state INTEGER NOT NULL DEFAULT 1,"
             "created_at INTEGER NOT NULL DEFAULT 0)"},
            {204, "source compat recharge order",
             "CREATE TABLE IF NOT EXISTS recharge_order("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "order_no TEXT NOT NULL UNIQUE,"
             "mch_id INTEGER NOT NULL,"
             "amount TEXT NOT NULL DEFAULT '0.00',"
             "pay_type TEXT NOT NULL DEFAULT '',"
             "state INTEGER NOT NULL DEFAULT 0,"
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "paid_at INTEGER NOT NULL DEFAULT 0)"},
            {205, "source compat merchant ext columns",
             "ALTER TABLE merchant ADD COLUMN invite_mch_id INTEGER NOT NULL DEFAULT 0"},
            {206, "source compat merchant realname columns",
             "ALTER TABLE merchant ADD COLUMN is_real_name INTEGER NOT NULL DEFAULT 0"},
            {207, "source compat merchant google columns",
             "ALTER TABLE merchant ADD COLUMN google_secret TEXT NOT NULL DEFAULT ''"},
            {208, "idx source compat domain mch",
             "CREATE INDEX IF NOT EXISTS idx_merchant_domain_mch ON merchant_domain(mch_id,state)"},
            {209, "idx source compat ticket mch",
             "CREATE INDEX IF NOT EXISTS idx_support_ticket_mch ON support_ticket(mch_id,state)"},
            {210, "idx source compat recharge mch",
             "CREATE INDEX IF NOT EXISTS idx_recharge_order_mch ON recharge_order(mch_id,state)"},
            {211, "source compat ticket category",
             "CREATE TABLE IF NOT EXISTS ticket_category("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "name TEXT NOT NULL,"
             "sort_order INTEGER NOT NULL DEFAULT 0,"
             "state INTEGER NOT NULL DEFAULT 1,"
             "created_at INTEGER NOT NULL DEFAULT 0)"},
            {212, "source compat register order",
             "CREATE TABLE IF NOT EXISTS register_order("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "order_no TEXT NOT NULL UNIQUE,"
             "username TEXT NOT NULL,"
             "password TEXT NOT NULL DEFAULT '',"
             "mch_name TEXT NOT NULL DEFAULT '',"
             "email TEXT NOT NULL DEFAULT '',"
             "phone TEXT NOT NULL DEFAULT '',"
             "amount TEXT NOT NULL DEFAULT '0.00',"
             "state INTEGER NOT NULL DEFAULT 0,"
             "mch_id INTEGER NOT NULL DEFAULT 0,"
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "paid_at INTEGER NOT NULL DEFAULT 0)"},
            {213, "source compat channel alert",
             "CREATE TABLE IF NOT EXISTS channel_alert("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "mch_id INTEGER NOT NULL DEFAULT 0,"
             "channel_id INTEGER NOT NULL DEFAULT 0,"
             "alert_type TEXT NOT NULL DEFAULT '',"
             "title TEXT NOT NULL DEFAULT '',"
             "content TEXT NOT NULL DEFAULT '',"
             "state INTEGER NOT NULL DEFAULT 0,"
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "handled_at INTEGER NOT NULL DEFAULT 0)"},
            {214, "source compat default ticket category",
             "INSERT OR IGNORE INTO ticket_category(id,name,sort_order,state,created_at) VALUES"
             "(1,'支付问题',10,1,0),"
             "(2,'账户问题',20,1,0),"
             "(3,'通道问题',30,1,0),"
             "(4,'其他问题',99,1,0)"},
            {215, "source compat merchant vip columns",
             "ALTER TABLE merchant ADD COLUMN vip_expire_at INTEGER NOT NULL DEFAULT 0"},
            {216, "pay_order add qrcode",
             "ALTER TABLE pay_order ADD COLUMN qrcode TEXT NOT NULL DEFAULT ''"},
            {217, "pay_order add raw_response",
             "ALTER TABLE pay_order ADD COLUMN raw_response TEXT NOT NULL DEFAULT ''"},
            {218, "default vmq codepay plugins",
             "INSERT OR IGNORE INTO plugin_store(plugin_code,display_name,description,vendor,"
             "version,icon,plugin_type,supported_ways,price,installed,state,created_at) VALUES"
             "('vmq','V免签','无需部署外部V免签，内置收款码/金额浮动/监听推送匹配','WePay-Cpp','1.0.0','📱',1,'wxpay,alipay,qqpay',0,1,1,0),"
             "('codepay','内置码支付','无需外部码支付服务，兼容checkOrder/checkPayResult/mpayNotify协议','WePay-Cpp','1.0.0','💬',1,'wxpay,alipay,qqpay',0,1,1,0)"},
            {219, "pay_qrcode add plugin_code",
             "ALTER TABLE pay_qrcode ADD COLUMN plugin_code TEXT NOT NULL DEFAULT 'vmq'"},
            {220, "pay_qrcode add channel_id",
             "ALTER TABLE pay_qrcode ADD COLUMN channel_id INTEGER NOT NULL DEFAULT 0"},
            {221, "pay_qrcode add qr_name",
             "ALTER TABLE pay_qrcode ADD COLUMN qr_name TEXT NOT NULL DEFAULT ''"},
            {222, "pay_qrcode add remark",
             "ALTER TABLE pay_qrcode ADD COLUMN remark TEXT NOT NULL DEFAULT ''"},
            {223, "vmq codepay params schema",
             "UPDATE plugin_store SET params_schema='["
             "{\"key\":\"lock_price\",\"label\":\"启用金额浮动\",\"type\":\"switch\",\"default\":true},"
             "{\"key\":\"require_online\",\"label\":\"要求监听端在线\",\"type\":\"switch\",\"default\":false},"
             "{\"key\":\"account\",\"label\":\"指定收款账号\",\"type\":\"input\",\"placeholder\":\"留空则自动选择\"},"
             "{\"key\":\"qrcode_url\",\"label\":\"兜底收款码URL\",\"type\":\"textarea\",\"placeholder\":\"没有收款码记录时使用\"}"
             "]' WHERE plugin_code IN ('vmq','codepay')"},
            {224, "default monitor settings",
             "INSERT OR IGNORE INTO setting(vkey,vvalue) VALUES"
             "('monitor_app_name','vmq'),"
             "('monitor_online_seconds','180'),"
             "('monitor_android_download_url',''),"
             "('monitor_ios_download_url','')"},
            {225, "merchant add vmq_key",
             "ALTER TABLE merchant ADD COLUMN vmq_key TEXT NOT NULL DEFAULT ''"},
            {226, "merchant add vmq_last_heart",
             "ALTER TABLE merchant ADD COLUMN vmq_last_heart INTEGER NOT NULL DEFAULT 0"},
            {227, "merchant add vmq_last_pay",
             "ALTER TABLE merchant ADD COLUMN vmq_last_pay INTEGER NOT NULL DEFAULT 0"},
            {228, "merchant add vmq_state",
             "ALTER TABLE merchant ADD COLUMN vmq_state INTEGER NOT NULL DEFAULT 0"},
            {229, "pay_channel add time_start",
             "ALTER TABLE pay_channel ADD COLUMN time_start TEXT NOT NULL DEFAULT ''"},
            {230, "pay_channel add time_stop",
             "ALTER TABLE pay_channel ADD COLUMN time_stop TEXT NOT NULL DEFAULT ''"},
            {231, "pay_channel add day_amount",
             "ALTER TABLE pay_channel ADD COLUMN day_amount TEXT NOT NULL DEFAULT '0'"},
            {232, "pay_channel add day_count",
             "ALTER TABLE pay_channel ADD COLUMN day_count INTEGER NOT NULL DEFAULT 0"},
            {233, "merchant add codepay_last_heart",
             "ALTER TABLE merchant ADD COLUMN codepay_last_heart INTEGER NOT NULL DEFAULT 0"},
            {234, "merchant add codepay_last_pay",
             "ALTER TABLE merchant ADD COLUMN codepay_last_pay INTEGER NOT NULL DEFAULT 0"},
            {235, "merchant add codepay_state",
             "ALTER TABLE merchant ADD COLUMN codepay_state INTEGER NOT NULL DEFAULT 0"},
            {236, "default monitor sub-plugins",
             "INSERT OR IGNORE INTO plugin_store(plugin_code,display_name,description,vendor,"
             "version,icon,plugin_type,supported_ways,price,installed,state,created_at) VALUES"
             "('alipay_monitor','支付宝免签','仅支付宝通道，订单池独立隔离','WePay-Cpp','1.0.0','💙',1,'alipay',0,0,1,0),"
             "('wx_monitor','微信免签','仅微信通道，订单池独立隔离','WePay-Cpp','1.0.0','💚',1,'wxpay',0,0,1,0),"
             "('qq_monitor','QQ免签','仅QQ通道，订单池独立隔离','WePay-Cpp','1.0.0','💜',1,'qqpay',0,0,1,0)"},

            // v237+: agpay 风格插件图标（bg_color 彩色背景）
            {237, "plugin_store add bg_color",
             "ALTER TABLE plugin_store ADD COLUMN bg_color TEXT NOT NULL DEFAULT ''"},
            {238, "plugin_store default bg_color",
             "UPDATE plugin_store SET bg_color=CASE plugin_code "
             "WHEN 'wxpay_native' THEN '#04BE02' WHEN 'wxpay_ext' THEN '#04BE02' "
             "WHEN 'alipay' THEN '#1677FF' WHEN 'alipay_ext' THEN '#1677FF' "
             "WHEN 'unionpay' THEN '#E60012' WHEN 'epay_upstream' THEN '#FF6A00' "
             "WHEN 'monitor' THEN '#7C3AED' WHEN 'vmq' THEN '#7C3AED' "
             "WHEN 'codepay' THEN '#6366F1' WHEN 'allinpay' THEN '#E65100' "
             "WHEN 'dgpay' THEN '#0F3E66' WHEN 'lklpay' THEN '#0066CC' "
             "WHEN 'hkrtpay' THEN '#2D2F92' WHEN 'jlpay' THEN '#F59E0B' "
             "WHEN 'lcswpay' THEN '#00AFFE' WHEN 'lespay' THEN '#FFE353' "
             "WHEN 'pppay' THEN '#EC4899' WHEN 'sxfpay' THEN '#78350F' "
             "WHEN 'umspay' THEN '#1E0AE8' WHEN 'xxpay' THEN '#6B7280' "
             "WHEN 'ysepay' THEN '#2563EB' WHEN 'alipay_monitor' THEN '#1677FF' "
             "WHEN 'wx_monitor' THEN '#04BE02' WHEN 'qq_monitor' THEN '#12B7F5' "
             "ELSE '#6B7280' END WHERE bg_color=''"},
            {239, "plugin_store alipay_sandbox",
             "INSERT OR IGNORE INTO plugin_store(plugin_code,display_name,description,vendor,"
             "version,icon,plugin_type,supported_ways,price,installed,state,created_at,bg_color) VALUES"
             "('alipay_sandbox','支付宝沙箱','支付宝沙箱环境，用于开发测试(需沙箱AppID/密钥/沙箱钱包App)','阿里','1.0.0','🧪',1,'ali_qr,ali_wap',0,0,1,0,'#FF6A00')"},
            {240, "callback_log add plugin column",
             "ALTER TABLE pay_callback_log ADD COLUMN plugin TEXT DEFAULT ''"},
            {241, "plugin_store wepay",
             "INSERT OR IGNORE INTO plugin_store(plugin_code,display_name,description,vendor,"
             "version,icon,plugin_type,supported_ways,price,installed,state,created_at,bg_color) VALUES"
             "('wepay','WePay 原生监控','WePay 自研监控支付插件，使用新版 API 协议(/api/wepay)，"
             "HMAC-SHA256签名/防重放/设备绑定/订单级匹配','WePay','1.0.0','💎',1,"
             "'wxpay,alipay,qqpay',0,0,1,0,'#2563EB')"},
            {242, "monitor_device table",
             "CREATE TABLE IF NOT EXISTS monitor_device("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "device_id TEXT NOT NULL UNIQUE,"
             "device_name TEXT DEFAULT '',"
             "device_model TEXT DEFAULT '',"
             "ip TEXT DEFAULT '',"
             "state INTEGER NOT NULL DEFAULT 1,"
             "last_heart INTEGER DEFAULT 0,"
             "last_push INTEGER DEFAULT 0,"
             "push_count INTEGER DEFAULT 0,"
             "created_at INTEGER NOT NULL)"},
            {243, "nonce_cache table",
             "CREATE TABLE IF NOT EXISTS nonce_cache("
             "nonce TEXT PRIMARY KEY,"
             "created_at INTEGER NOT NULL)"},
            {244, "rename vmq display_name",
             "UPDATE plugin_store SET display_name='V免签' WHERE plugin_code='vmq'"},
            {2441, "rename codepay display_name",
             "UPDATE plugin_store SET display_name='内置码支付' WHERE plugin_code='codepay'"},
            {245, "caihongyi upstream plugin_store",
             "INSERT OR IGNORE INTO plugin_store(plugin_code,display_name,description,vendor,"
             "version,icon,plugin_type,supported_ways,price,installed,state,created_at) VALUES"
             "('adapay','汇付 Adapay',' adapay 插件的上游通道骨架','汇付','1.0.0','💳',1,'wxpay,alipay,bank',0,0,1,0),"
             "('alipaycode','支付宝当面付',' alipaycode 插件','支付宝','1.0.0','🔵',1,'alipay',0,0,1,0),"
             "('alipayd','支付宝直付通',' alipayd 插件','支付宝','1.0.0','🔵',1,'alipay',0,0,1,0),"
             "('alipayg','支付宝全球付',' alipayg 插件','支付宝','1.0.0','🔵',1,'alipay',0,0,1,0),"
             "('alipayhk','支付宝香港','alipayhk 插件','支付宝','1.0.0','🔵',1,'alipay',0,0,1,0),"
             "('alipayrp','支付宝红包',' alipayrp 插件','支付宝','1.0.0','🔵',1,'alipay',0,0,1,0),"
             "('alipaysl','支付宝服务商','alipaysl 插件','支付宝','1.0.0','🔵',1,'alipay',0,0,1,0),"
             "('chinaums','银联商务','chinaums 插件','银联商务','1.0.0','🏦',1,'wxpay,alipay,bank',0,0,1,0),"
             "('dinpay','智付',' dinpay 插件','智付','1.0.0','💳',1,'wxpay,alipay,bank',0,0,1,0),"
             "('duolabao','哆啦宝',' duolabao 插件','哆啦宝','1.0.0','💳',1,'wxpay,alipay',0,0,1,0),"
             "('easypay','易生易企通',' easypay 插件','易生','1.0.0','💳',1,'wxpay,alipay,bank',0,0,1,0),"
             "('epay','彩虹易支付','彩虹易支付上游协议插件','彩虹','1.0.0','🌈',1,'wxpay,alipay,qqpay,bank,jdpay',0,0,1,0),"
             "('epayn','彩虹易支付V2','彩虹易支付 V2 上游协议插件','彩虹','1.0.0','🌈',1,'wxpay,alipay,qqpay,bank,jdpay',0,0,1,0),"
             "('fubei','付呗',' fubei 插件','付呗','1.0.0','💳',1,'wxpay,alipay',0,0,1,0),"
             "('fuiou2','富友支付',' fuiou2 插件','富友','1.0.0','💳',1,'wxpay,alipay,bank',0,0,1,0),"
             "('haipay','嗨付',' haipay 插件','嗨付','1.0.0','💳',1,'wxpay,alipay',0,0,1,0),"
             "('heepay','汇付宝',' heepay 插件','汇付宝','1.0.0','💳',1,'wxpay,alipay,bank',0,0,1,0),"
             "('hlpay','海联支付',' hlpay 插件','海联','1.0.0','💳',1,'wxpay,alipay,bank',0,0,1,0),"
             "('hnapay','新生支付',' hnapay 插件','新生','1.0.0','💳',1,'wxpay,alipay,bank',0,0,1,0),"
             "('huifu','汇付斗拱平台',' huifu 插件','汇付','1.0.0','⚡',1,'wxpay,alipay,bank',0,0,1,0),"
             "('huolian','火脸支付',' huolian 插件','火脸','1.0.0','💳',1,'wxpay,alipay',0,0,1,0),"
             "('jdpay','京东支付',' jdpay 插件','京东','1.0.0','🛒',1,'jdpay,bank',0,0,1,0),"
             "('jeepay','计全支付',' jeepay 插件','计全','1.0.0','💳',1,'wxpay,alipay',0,0,1,0),"
             "('kayixin','开薪支付',' kayixin 插件','开薪','1.0.0','💳',1,'wxpay,alipay',0,0,1,0),"
             "('kuaiqian','快钱支付',' kuaiqian 插件','快钱','1.0.0','💳',1,'wxpay,alipay,bank',0,0,1,0),"
             "('lakala','拉卡拉',' lakala 插件','拉卡拉','1.0.0','💳',1,'wxpay,alipay,bank',0,0,1,0),"
             "('leshua','乐刷',' leshua 插件','乐刷','1.0.0','💳',1,'wxpay,alipay',0,0,1,0),"
             "('ltzf','蓝兔支付',' ltzf 插件','蓝兔','1.0.0','💳',1,'wxpay,alipay',0,0,1,0),"
             "('lzyzf','领智云支付',' lzyzf 插件','领智云','1.0.0','💳',1,'wxpay,alipay',0,0,1,0),"
             "('passpay','PassPay',' passpay 插件','PassPay','1.0.0','💳',1,'wxpay,alipay,bank',0,0,1,0),"
             "('paypal','PayPal',' paypal 插件','PayPal','1.0.0','💙',1,'paypal',0,0,1,0),"
             "('qqpay','QQ钱包',' qqpay 插件','腾讯','1.0.0','💬',1,'qqpay',0,0,1,0),"
             "('sandpay','杉德支付',' sandpay 插件','杉德','1.0.0','💳',1,'wxpay,alipay,bank',0,0,1,0),"
             "('shengpay','盛付通',' shengpay 插件','盛付通','1.0.0','💳',1,'wxpay,alipay,bank',0,0,1,0),"
             "('stripe','Stripe',' stripe 插件','Stripe','1.0.0','💜',1,'card',0,0,1,0),"
             "('swiftpass','威富通RSA',' swiftpass 插件','威富通','1.0.0','💳',1,'wxpay,alipay,qqpay,bank',0,0,1,0),"
             "('swiftpass2','威富通',' swiftpass2 插件','威富通','1.0.0','💳',1,'wxpay,alipay,qqpay,bank',0,0,1,0),"
             "('umfpay','联动优势',' umfpay 插件','联动优势','1.0.0','💳',1,'wxpay,alipay,bank',0,0,1,0),"
             "('usdtpro','USDT',' usdtpro 插件','USDT','1.0.0','₮',1,'usdt',0,0,1,0),"
             "('woaizf','我爱支付',' woaizf 插件','我爱支付','1.0.0','💳',1,'wxpay,alipay',0,0,1,0),"
             "('wxpay','微信官方支付',' wxpay 插件','微信','1.0.0','💚',1,'wxpay,bank',0,0,1,0),"
             "('wxpayn','微信官方支付V3',' wxpayn 插件','微信','1.0.0','💚',1,'wxpay',0,0,1,0),"
             "('wxpayng','微信支付V3服务商',' wxpayng 插件','微信','1.0.0','💚',1,'wxpay',0,0,1,0),"
             "('wxpaynp','微信支付V3平台证书',' wxpaynp 插件','微信','1.0.0','💚',1,'wxpay',0,0,1,0),"
             "('wxpaysl','微信支付服务商',' wxpaysl 插件','微信','1.0.0','💚',1,'wxpay',0,0,1,0),"
             "('xorpay','XorPay',' xorpay 插件','XorPay','1.0.0','💳',1,'wxpay,alipay',0,0,1,0),"
             "('xsy','小树云',' xsy 插件','小树云','1.0.0','💳',1,'wxpay,alipay',0,0,1,0),"
             "('xunhupay','迅虎支付',' xunhupay 插件','迅虎','1.0.0','💳',1,'wxpay,alipay',0,0,1,0),"
             "('yeepay','易宝支付',' yeepay 插件','易宝','1.0.0','💳',1,'wxpay,alipay,bank',0,0,1,0),"
             "('yinyingtong','银盈通',' yinyingtong 插件','银盈通','1.0.0','💳',1,'wxpay,alipay,bank',0,0,1,0),"
             "('yseqt','银盛企通',' yseqt 插件','银盛','1.0.0','💳',1,'wxpay,alipay,bank',0,0,1,0),"
             "('zhangyishou','掌易收',' zhangyishou 插件','掌易收','1.0.0','💳',1,'wxpay,alipay',0,0,1,0),"
             "('zyu','卓越云',' zyu 插件','卓越云','1.0.0','💳',1,'wxpay,alipay',0,0,1,0)"},
            {246, "mark caihongyi external plugins",
             "UPDATE plugin_store SET plugin_type=2,installed=0,state=1,"
             "description=description||'（外部插件，需实现逐个）',"
             "params_schema='["
             "{\"key\":\"appurl\",\"label\":\"接口地址\",\"type\":\"input\",\"default\":\"\",\"help\":\"插件 appurl/API 地址\"},"
             "{\"key\":\"appid\",\"label\":\"商户ID/AppID\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appmchid\",\"label\":\"商户号/MchID\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"商户密钥/平台公钥/API密钥\",\"type\":\"textarea\",\"default\":\"\"},"
             "{\"key\":\"appsecret\",\"label\":\"商户私钥/AppSecret/APIv3密钥\",\"type\":\"textarea\",\"default\":\"\"},"
             "{\"key\":\"appswitch\",\"label\":\"模式/环境开关\",\"type\":\"select\",\"default\":\"0\","
             "\"options\":[{\"value\":\"0\",\"label\":\"默认/生产\"},{\"value\":\"1\",\"label\":\"启用/测试\"}]}"
             "]' "
             "WHERE plugin_code IN ("
             "'adapay','alipaycode','alipayd','alipayg','alipayhk','alipayrp','alipaysl',"
             "'chinaums','dinpay','duolabao','easypay','epay','epayn','fubei','fuiou2',"
             "'haipay','heepay','hlpay','hnapay','huifu','huolian','jdpay',"
             "'kayixin','kuaiqian','lakala','leshua','ltzf','lzyzf','passpay','paypal',"
             "'qqpay','sandpay','shengpay','stripe','swiftpass','swiftpass2','umfpay',"
             "'usdtpro','woaizf','wxpay','wxpayn','wxpayng','wxpaynp','wxpaysl','xorpay',"
             "'xsy','xunhupay','yeepay','yinyingtong','yseqt','zhangyishou','zyu')"},
            {247, "jeepay builtin plugin",
             "UPDATE plugin_store SET display_name='Jeepay聚合支付',"
             "description=' jeepay 插件：统一下单/MD5签名/回调验签/退款',"
             "vendor='Jeepay',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,bank',"
             "params_schema='["
             "{\"key\":\"appurl\",\"label\":\"接口地址\",\"type\":\"input\",\"default\":\"\",\"help\":\"必须以 http:// 或 https:// 开头，以 / 结尾\"},"
             "{\"key\":\"appmchid\",\"label\":\"商户号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appid\",\"label\":\"应用 AppId\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"私钥 AppSecret\",\"type\":\"textarea\",\"default\":\"\"},"
             "{\"key\":\"apptype\",\"label\":\"支付模式\",\"type\":\"input\",\"default\":\"1\",\"help\":\"彩虹易 apptype：1扫码 2/H5/PC 3/JSAPI/WAP 4小程序 5聚合扫码 6WEB收银台 7APP，可逗号分隔\"}"
             "]' WHERE plugin_code='jeepay'"},
            {248, "adapay builtin plugin",
             "UPDATE plugin_store SET display_name='AdaPay聚合支付',"
             "description=' adapay 插件：AdaPay API/SHA1withRSA/回调验签/退款',"
             "vendor='AdaPay',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,bank',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"应用 App_ID\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"prod 模式 API_KEY\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appsecret\",\"label\":\"商户 RSA 私钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"可填写去掉 BEGIN/END 的裸私钥\"},"
             "{\"key\":\"apptype\",\"label\":\"支付模式\",\"type\":\"input\",\"default\":\"1\",\"help\":\"支付宝:1扫码 2JS 3托管小程序；微信:1自有公众号/小程序 2动态二维码 3托管小程序；银行:1银联 2快捷 3网银\"}"
             "]' WHERE plugin_code='adapay'"},
            {249, "alipay caihongyi schema",
             "UPDATE plugin_store SET display_name='支付宝官方支付',"
             "description=' alipay 插件完善：电脑网站/手机网站/当面付扫码/APP/JSAPI字段兼容',"
             "vendor='支付宝',plugin_type=1,state=1,"
             "supported_ways='alipay,ali_qr,ali_wap,ali_page,ali_app',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"应用APPID\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"支付宝公钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"填错可支付但无法回调；证书模式可留空\"},"
             "{\"key\":\"appsecret\",\"label\":\"应用私钥\",\"type\":\"textarea\",\"default\":\"\"},"
             "{\"key\":\"appmchid\",\"label\":\"卖家支付宝用户ID\",\"type\":\"input\",\"default\":\"\",\"help\":\"可留空，默认商户签约账号\"},"
             "{\"key\":\"gateway\",\"label\":\"网关地址\",\"type\":\"input\",\"default\":\"https://openapi.alipay.com/gateway.do\"},"
             "{\"key\":\"apptype\",\"label\":\"可用接口\",\"type\":\"input\",\"default\":\"3\",\"help\":\"1电脑网站 2手机网站 3当面付扫码 4当面付JS 5预授权 6APP 7JSAPI 8订单码，可逗号分隔\"}"
             "]' WHERE plugin_code='alipay'"},
            {250, "alipaycode builtin plugin",
             "UPDATE plugin_store SET display_name='支付宝免签约码支付',"
             "description=' alipaycode 插件：普通转账/转账确认单，需账单监听匹配到账',"
             "vendor='支付宝',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"应用APPID\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"支付宝公钥\",\"type\":\"textarea\",\"default\":\"\"},"
             "{\"key\":\"appsecret\",\"label\":\"应用私钥\",\"type\":\"textarea\",\"default\":\"\"},"
             "{\"key\":\"appmchid\",\"label\":\"支付宝UID\",\"type\":\"input\",\"default\":\"\",\"help\":\"2088开头的16位支付宝用户ID\"},"
             "{\"key\":\"apptoken\",\"label\":\"商户授权token\",\"type\":\"input\",\"default\":\"\",\"help\":\"第三方应用填写，非第三方应用留空\"},"
             "{\"key\":\"appswitch\",\"label\":\"支付类型\",\"type\":\"select\",\"default\":\"0\","
             "\"options\":[{\"value\":\"0\",\"label\":\"普通转账\"},{\"value\":\"1\",\"label\":\"转账确认单\"}]}"
             "]' WHERE plugin_code='alipaycode'"},
            {251, "alipayd builtin plugin",
             "UPDATE plugin_store SET display_name='支付宝官方支付直付通版',"
             "description=' alipayd 插件：互联网平台直付通，注入 sub_merchant 与 settle_info',"
             "vendor='支付宝',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,ali_qr,ali_wap,ali_page,ali_app',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"应用APPID\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"支付宝公钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"填错可支付但无法回调；证书模式可留空\"},"
             "{\"key\":\"appsecret\",\"label\":\"应用私钥\",\"type\":\"textarea\",\"default\":\"\"},"
             "{\"key\":\"appmchid\",\"label\":\"子商户SMID\",\"type\":\"input\",\"default\":\"\",\"help\":\"多个 SMID 可用英文逗号分隔\"},"
             "{\"key\":\"gateway\",\"label\":\"网关地址\",\"type\":\"input\",\"default\":\"https://openapi.alipay.com/gateway.do\"},"
             "{\"key\":\"apptype\",\"label\":\"可用接口\",\"type\":\"input\",\"default\":\"3\",\"help\":\"1电脑网站 2手机网站 3当面付扫码 4当面付JS 5预授权 6APP 7JSAPI 8订单码，可逗号分隔\"}"
             "]' WHERE plugin_code='alipayd'"},
            {252, "alipayg builtin plugin",
             "UPDATE plugin_store SET display_name='支付宝国际版',"
             "description=' alipayg 插件：Antom CASHIER_PAYMENT/RSA256 请求头签名/退款/关闭',"
             "vendor='Antom',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"应用Client ID\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"Antom公钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"用于回调/响应验签\"},"
             "{\"key\":\"appsecret\",\"label\":\"应用私钥\",\"type\":\"textarea\",\"default\":\"\"},"
             "{\"key\":\"appswitch\",\"label\":\"网关区域\",\"type\":\"select\",\"default\":\"0\","
             "\"options\":[{\"value\":\"0\",\"label\":\"亚洲 open-sea-global\"},{\"value\":\"1\",\"label\":\"北美 open-na-global\"},{\"value\":\"2\",\"label\":\"欧洲 open-de-global\"}]},"
             "{\"key\":\"currency_code\",\"label\":\"结算货币\",\"type\":\"input\",\"default\":\"CNY\"},"
             "{\"key\":\"currency_rate\",\"label\":\"货币汇率\",\"type\":\"input\",\"default\":\"1\"}"
             "]' WHERE plugin_code='alipayg'"},
            {253, "alipayhk builtin plugin",
             "UPDATE plugin_store SET display_name='AlipayHK',"
             "description=' alipayhk 插件：旧版国际支付宝 create_forex_trade/create_forex_trade_wap/mobile.securitypay.pay',"
             "vendor='支付宝',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,ali_wap,ali_app',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"Partner ID\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"MD5 Key\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appswitch\",\"label\":\"支付时选择钱包类型\",\"type\":\"select\",\"default\":\"0\","
             "\"options\":[{\"value\":\"0\",\"label\":\"否\"},{\"value\":\"1\",\"label\":\"是\"}]},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"input\",\"default\":\"1\",\"help\":\"1=PC支付 2=WAP支付 3=APP支付\"},"
             "{\"key\":\"gateway\",\"label\":\"网关地址\",\"type\":\"input\",\"default\":\"https://intlmapi.alipay.com/gateway.do\"}"
             "]' WHERE plugin_code='alipayhk'"},
            {254, "alipayrp builtin plugin",
             "UPDATE plugin_store SET display_name='支付宝现金红包',"
             "description=' alipayrp 插件：alipay.fund.trans.app.pay 红包付款、资金单据通知、红包退款',"
             "vendor='支付宝',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"应用APPID\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appsecret\",\"label\":\"应用私钥\",\"type\":\"textarea\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"支付宝公钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"用于资金单据状态变更通知验签\"},"
             "{\"key\":\"appmchid\",\"label\":\"收款方支付宝UID\",\"type\":\"input\",\"default\":\"\",\"help\":\"留空则需业务侧提供 payee_user_id\"},"
             "{\"key\":\"gateway\",\"label\":\"网关地址\",\"type\":\"input\",\"default\":\"https://openapi.alipay.com/gateway.do\"}"
             "]' WHERE plugin_code='alipayrp'"},
            {323, "notify_task add plugin column",
             "ALTER TABLE pay_notify_task ADD COLUMN plugin TEXT DEFAULT ''"},
            {255, "allinpay caihongyi schema",
             "UPDATE plugin_store SET display_name='通联支付',"
             "description=' allinpay 插件：unitorder/pay、RSA签名、A/W/Q/U 支付类型、退款与回调验签',"
             "vendor='通联',plugin_type=1,state=1,"
             "supported_ways='alipay,wxpay,qqpay,bank',"
             "params_schema='["
             "{\"key\":\"appmchid\",\"label\":\"商户号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appid\",\"label\":\"应用ID\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"通联公钥\",\"type\":\"textarea\",\"default\":\"\"},"
             "{\"key\":\"appsecret\",\"label\":\"商户私钥\",\"type\":\"textarea\",\"default\":\"\"},"
             "{\"key\":\"orgid\",\"label\":\"代理商商户号\",\"type\":\"input\",\"default\":\"\",\"help\":\"仅代理商需要填写\"},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"input\",\"default\":\"1\",\"help\":\"1=扫码支付 2=JS支付/公众号/小程序\"}"
             "]' WHERE plugin_code='allinpay'"},
            {256, "chinaums builtin plugin",
             "UPDATE plugin_store SET display_name='银联商务',"
             "description=' chinaums 插件：OPEN-BODY-SIG/HMAC-SHA256、二维码/H5/H5转小程序、回调验签',"
             "vendor='银联商务',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,bank',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"AppId\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"AppKey\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appmchid\",\"label\":\"商户号mid\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appurl\",\"label\":\"终端号tid\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appsecret\",\"label\":\"通讯密钥\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"msgsrcid\",\"label\":\"来源编号\",\"type\":\"input\",\"default\":\"\",\"help\":\"4位来源编号，拼接到订单号前\"},"
             "{\"key\":\"appswitch\",\"label\":\"环境选择\",\"type\":\"select\",\"default\":\"0\","
             "\"options\":[{\"value\":\"0\",\"label\":\"生产环境\"},{\"value\":\"1\",\"label\":\"测试环境\"}]},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"input\",\"default\":\"1\",\"help\":\"支付宝/微信：1扫码 2H5；微信：3H5转小程序\"}"
             "]' WHERE plugin_code='chinaums'"},
            {257, "dinpay registered plugin",
             "UPDATE plugin_store SET display_name='智付',"
             "description=' dinpay 参数注册；协议依赖 SM2/SM4 国密加密签名，需接入国密实现后启用真实下单',"
             "vendor='智付',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,bank',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"商户号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appsecret\",\"label\":\"商户私钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"SM2-Hex格式\"},"
             "{\"key\":\"appkey\",\"label\":\"平台公钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"SM2-Hex格式\"},"
             "{\"key\":\"appmchid\",\"label\":\"子商户号\",\"type\":\"input\",\"default\":\"\",\"help\":\"可留空\"},"
             "{\"key\":\"reportid\",\"label\":\"渠道商户报备ID\",\"type\":\"input\",\"default\":\"\",\"help\":\"可留空，多个报备ID可用英文逗号分隔\"},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"input\",\"default\":\"1\",\"help\":\"支付宝/微信：1扫码 2H5 3JS\"},"
             "{\"key\":\"appswitch\",\"label\":\"环境\",\"type\":\"select\",\"default\":\"0\","
             "\"options\":[{\"value\":\"0\",\"label\":\"生产环境\"},{\"value\":\"1\",\"label\":\"测试环境\"}]}"
             "]' WHERE plugin_code='dinpay'"},
            {258, "duolabao builtin plugin",
             "UPDATE plugin_store SET display_name='哆啦宝支付',"
             "description=' duolabao 插件：SHA1 token、二维码/JS支付、JSON回调验签、退款',"
             "vendor='哆啦宝',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,qqpay,bank,jdpay',"
             "params_schema='["
             "{\"key\":\"agentNum\",\"label\":\"代理商编号\",\"type\":\"input\",\"default\":\"\",\"help\":\"非代理商不需要填写\"},"
             "{\"key\":\"customerNum\",\"label\":\"商户编号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"shopNum\",\"label\":\"店铺编号\",\"type\":\"input\",\"default\":\"\",\"help\":\"可留空\"},"
             "{\"key\":\"accessKey\",\"label\":\"公钥\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"secretKey\",\"label\":\"私钥\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"input\",\"default\":\"1\",\"help\":\"支付宝/微信：1扫码 2JS\"}"
             "]' WHERE plugin_code='duolabao'"},
            {259, "easypay builtin plugin",
             "UPDATE plugin_store SET display_name='易生易企通',"
             "description=' easypay 插件：RSA-SHA256，trade/native、trade/jsapi、退款与JSON回调验签',"
             "vendor='易生',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,bank',"
             "params_schema='["
             "{\"key\":\"reqtype\",\"label\":\"接入模式\",\"type\":\"select\",\"default\":\"2\","
             "\"options\":[{\"value\":\"2\",\"label\":\"机构模式\"},{\"value\":\"1\",\"label\":\"商户模式\"}]},"
             "{\"key\":\"appid\",\"label\":\"机构号/商户号\",\"type\":\"input\",\"default\":\"\",\"help\":\"reqId\"},"
             "{\"key\":\"appmchid\",\"label\":\"子商户号\",\"type\":\"input\",\"default\":\"\",\"help\":\"机构模式填写子商户号，商户模式留空\"},"
             "{\"key\":\"appkey\",\"label\":\"易生公钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"不能有换行和标签\"},"
             "{\"key\":\"appsecret\",\"label\":\"商户私钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"不能有换行和标签\"},"
             "{\"key\":\"appswitch\",\"label\":\"环境选择\",\"type\":\"select\",\"default\":\"0\","
             "\"options\":[{\"value\":\"0\",\"label\":\"生产环境\"},{\"value\":\"1\",\"label\":\"测试环境\"}]},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"input\",\"default\":\"1\",\"help\":\"支付宝/云闪付：1主扫 2JSAPI\"}"
             "]' WHERE plugin_code='easypay'"},
            {260, "epay builtin plugin",
             "UPDATE plugin_store SET display_name='彩虹易支付',"
             "description=' epay 插件：submit.php/mapi.php/api.php，MD5签名、回调验签、查询和退款',"
             "vendor='彩虹',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,qqpay,wxpay,bank,jdpay',"
             "params_schema='["
             "{\"key\":\"appurl\",\"label\":\"接口地址\",\"type\":\"input\",\"default\":\"\",\"help\":\"必须以 http:// 或 https:// 开头，以 / 结尾\"},"
             "{\"key\":\"appid\",\"label\":\"商户ID\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"商户密钥\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appswitch\",\"label\":\"是否使用mapi接口\",\"type\":\"select\",\"default\":\"0\","
             "\"options\":[{\"value\":\"0\",\"label\":\"否\"},{\"value\":\"1\",\"label\":\"是\"}]}"
             "]' WHERE plugin_code='epay'"},
            {261, "epayn builtin plugin",
             "UPDATE plugin_store SET display_name='彩虹易支付V2',"
             "description=' epayn 插件：RSA-SHA256，api/pay/create/query/refund，timestamp回调验签',"
             "vendor='彩虹',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,qqpay,wxpay,bank,jdpay',"
             "params_schema='["
             "{\"key\":\"appurl\",\"label\":\"接口地址\",\"type\":\"input\",\"default\":\"\",\"help\":\"必须以 http:// 或 https:// 开头，以 / 结尾\"},"
             "{\"key\":\"appid\",\"label\":\"商户ID\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"平台公钥\",\"type\":\"textarea\",\"default\":\"\"},"
             "{\"key\":\"appsecret\",\"label\":\"商户私钥\",\"type\":\"textarea\",\"default\":\"\"},"
             "{\"key\":\"appswitch\",\"label\":\"是否使用mapi接口\",\"type\":\"select\",\"default\":\"0\","
             "\"options\":[{\"value\":\"0\",\"label\":\"否\"},{\"value\":\"1\",\"label\":\"是\"}]}"
             "]' WHERE plugin_code='epayn'"},
            {262, "fubei builtin plugin",
             "UPDATE plugin_store SET display_name='付呗聚合支付',"
             "description=' fubei 插件：fbpay.order.create/wap/refund，MD5大写签名，JSON回调验签',"
             "vendor='付呗',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,bank',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"开放平台ID\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"接口密钥\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appmchid\",\"label\":\"门店ID\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"mchid\",\"label\":\"商户ID\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"apptype\",\"label\":\"支付宝支付方式\",\"type\":\"input\",\"default\":\"1\",\"help\":\"1=生活号支付 2=H5支付\"}"
             "]' WHERE plugin_code='fubei'"},
            {263, "fuiou2 builtin plugin",
             "UPDATE plugin_store SET display_name='富友支付(合作方)',"
             "description=' fuiou2 插件：XML req双URL编码、RSA-MD5签名、preCreate/wxPreCreate/commonRefund、XML回调验签',"
             "vendor='富友',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,bank',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"机构号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appmchid\",\"label\":\"商户号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appsecret\",\"label\":\"商户私钥\",\"type\":\"textarea\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"富友公钥\",\"type\":\"textarea\",\"default\":\"\"},"
             "{\"key\":\"appurl\",\"label\":\"订单号前缀\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"entrykey\",\"label\":\"代理进件密钥\",\"type\":\"input\",\"default\":\"\",\"help\":\"不使用进件或投诉接口可不填写\"},"
             "{\"key\":\"appswitch\",\"label\":\"环境选择\",\"type\":\"select\",\"default\":\"0\","
             "\"options\":[{\"value\":\"0\",\"label\":\"生产环境\"},{\"value\":\"1\",\"label\":\"测试环境\"}]},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"input\",\"default\":\"1\",\"help\":\"1=扫码支付 2=公众号/小程序支付\"}"
             "]' WHERE plugin_code='fuiou2'"},
            {264, "alipaysl builtin plugin",
             "UPDATE plugin_store SET display_name='支付宝官方支付服务商版',"
             "description=' alipaysl 插件：第三方应用服务商 app_auth_token，电脑/手机/扫码/JSAPI/APP，RSA2签名验签、退款和关闭',"
             "vendor='支付宝',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,ali_wap,ali_app,ali_jsapi',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"应用APPID\",\"type\":\"input\",\"default\":\"\",\"help\":\"必须使用第三方应用\"},"
             "{\"key\":\"appkey\",\"label\":\"支付宝公钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"公钥证书模式可留空\"},"
             "{\"key\":\"appsecret\",\"label\":\"应用私钥\",\"type\":\"textarea\",\"default\":\"\"},"
             "{\"key\":\"appmchid\",\"label\":\"商户授权token\",\"type\":\"input\",\"default\":\"\",\"help\":\"app_auth_token\"},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"input\",\"default\":\"3\",\"help\":\"1=电脑网站 2=手机网站 3=扫码 4=当面付JS 6=APP 7=JSAPI 8=订单码\"},"
             "{\"key\":\"gateway\",\"label\":\"支付宝网关\",\"type\":\"input\",\"default\":\"https://openapi.alipay.com/gateway.do\"}"
             "]' WHERE plugin_code='alipaysl'"},
            {265, "haipay builtin plugin",
             "UPDATE plugin_store SET display_name='海科聚合支付',"
             "description=' haipay 插件：递归MD5大写签名，pre-pay/passive-pay/order-query/close-order/refund，JSON回调验签',"
             "vendor='海科融通',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,bank',"
             "params_schema='["
             "{\"key\":\"accessid\",\"label\":\"accessid\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"accesskey\",\"label\":\"接入秘钥\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"agent_no\",\"label\":\"服务商编号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"merch_no\",\"label\":\"商户编号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"pn\",\"label\":\"终端号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"input\",\"default\":\"1\",\"help\":\"支付宝：1扫码 2JS支付\"},"
             "{\"key\":\"appswitch\",\"label\":\"环境\",\"type\":\"select\",\"default\":\"0\","
             "\"options\":[{\"value\":\"0\",\"label\":\"生产环境\"},{\"value\":\"1\",\"label\":\"测试环境\"}]}"
             "]' WHERE plugin_code='haipay'"},
            {266, "heepay builtin plugin",
             "UPDATE plugin_store SET display_name='汇付宝',"
             "description=' heepay 插件：固定字段顺序MD5签名，Payment/Index.aspx跳转支付、回调验签、PaymentRefund退款',"
             "vendor='汇付宝',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,bank',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"商户编号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appmchid\",\"label\":\"二级商户号\",\"type\":\"input\",\"default\":\"\",\"help\":\"可留空，集团模式传参\"},"
             "{\"key\":\"bank_id\",\"label\":\"上游商户BankId\",\"type\":\"input\",\"default\":\"\",\"help\":\"可留空，多个用英文逗号分隔\"},"
             "{\"key\":\"appkey\",\"label\":\"支付密钥\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appsecret\",\"label\":\"退款密钥\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"transfer_key\",\"label\":\"付款密钥\",\"type\":\"input\",\"default\":\"\",\"help\":\"不需要付款功能可留空\"},"
             "{\"key\":\"transfer_des_key\",\"label\":\"付款3DES加密密钥\",\"type\":\"input\",\"default\":\"\",\"help\":\"不需要付款功能可留空\"},"
             "{\"key\":\"mch_private_key\",\"label\":\"常规业务RSA私钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"不需要投诉查询功能可留空\"},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"input\",\"default\":\"1\",\"help\":\"微信：1扫码 2小程序H5；银行卡：1网银 2银联 3云闪付H5\"}"
             "]' WHERE plugin_code='heepay'"},
            {267, "hlpay builtin plugin",
             "UPDATE plugin_store SET display_name='汇联支付',"
             "description=' hlpay 插件：RSA2签名，openapi/pay/create/refund，响应与JSON回调深度排序验签',"
             "vendor='汇联',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,bank',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"应用APPID\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"商户私钥\",\"type\":\"textarea\",\"default\":\"\"},"
             "{\"key\":\"appsecret\",\"label\":\"平台公钥\",\"type\":\"textarea\",\"default\":\"\"},"
             "{\"key\":\"channelcode\",\"label\":\"通道编码\",\"type\":\"input\",\"default\":\"\",\"help\":\"可留空，留空为随机路由\"},"
             "{\"key\":\"appmchid\",\"label\":\"子商户编码\",\"type\":\"input\",\"default\":\"\",\"help\":\"仅服务商可传，普通商户请勿填写\"},"
             "{\"key\":\"appswitch\",\"label\":\"场景类型\",\"type\":\"select\",\"default\":\"1\","
             "\"options\":[{\"value\":\"1\",\"label\":\"线下\"},{\"value\":\"2\",\"label\":\"线上\"}]},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"input\",\"default\":\"1\",\"help\":\"支付宝：1扫码 2JS 3PC 4H5；微信：1扫码 2公众号/小程序\"}"
             "]' WHERE plugin_code='hlpay'"},
            {268, "hnapay builtin plugin",
             "UPDATE plugin_store SET display_name='新生支付',"
             "description=' hnapay 插件：扫码/JSAPI/H5，RSA-SHA1签名，RSA公钥分段加密msgCiphertext，回调验签与退款',"
             "vendor='新生支付',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,bank',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"商户ID\",\"type\":\"input\",\"default\":\"\",\"help\":\"新生用户ID\"},"
             "{\"key\":\"appkey\",\"label\":\"新生公钥(新收款密钥)\",\"type\":\"textarea\",\"default\":\"\"},"
             "{\"key\":\"appsecret\",\"label\":\"商户私钥(新收款密钥)\",\"type\":\"textarea\",\"default\":\"\"},"
             "{\"key\":\"appmchid\",\"label\":\"报备编号\",\"type\":\"input\",\"default\":\"\",\"help\":\"仅支付宝/微信需要填写\"},"
             "{\"key\":\"appswitch\",\"label\":\"接口类型\",\"type\":\"select\",\"default\":\"2\","
             "\"options\":[{\"value\":\"0\",\"label\":\"公众号/生活号支付\"},{\"value\":\"1\",\"label\":\"支付宝H5\"},{\"value\":\"2\",\"label\":\"扫码支付\"}]}"
             "]' WHERE plugin_code='hnapay'"},
            {269, "huifu builtin plugin",
             "UPDATE plugin_store SET display_name='汇付斗拱平台',"
             "description=' huifu 插件：斗拱JSON API，data排序JSON后RSA-SHA256签名，jspay统一下单、回调验签、退款和关单',"
             "vendor='汇付天下',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,bank,ecny',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"汇付系统号\",\"type\":\"input\",\"default\":\"\",\"help\":\"主体为渠道商填渠道商ID，主体为直连商户填商户ID\"},"
             "{\"key\":\"appurl\",\"label\":\"汇付产品号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appsecret\",\"label\":\"商户私钥\",\"type\":\"textarea\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"汇付公钥\",\"type\":\"textarea\",\"default\":\"\"},"
             "{\"key\":\"appmchid\",\"label\":\"汇付子商户号\",\"type\":\"input\",\"default\":\"\",\"help\":\"主体为渠道商时填写，直连商户可留空\"},"
             "{\"key\":\"project_id\",\"label\":\"半支付托管项目号\",\"type\":\"input\",\"default\":\"\",\"help\":\"仅托管H5/PC支付需要填写\"},"
             "{\"key\":\"seq_id\",\"label\":\"托管小程序应用ID\",\"type\":\"input\",\"default\":\"\",\"help\":\"仅托管小程序支付可填写\"},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"input\",\"default\":\"1\",\"help\":\"支付宝:1扫码 2托管H5/PC 3托管小程序 4JS；微信:1自有JSAPI 2托管H5/PC 3托管小程序；银行卡:1银联扫码 2快捷 3网银 4银联JS\"}"
             "]' WHERE plugin_code='huifu'"},
            {270, "huolian builtin plugin",
             "UPDATE plugin_store SET display_name='火脸支付',"
             "description=' huolian 插件：open.lianok.com统一JSON网关，参数排序转小写后MD5加盐签名，聚合/H5/小程序下单、回调验签、退款',"
             "vendor='火脸',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,bank',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"对接商授权编号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"对接商MD5加密盐\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appmchid\",\"label\":\"商户ID\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appurl\",\"label\":\"收银员手机号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appsecret\",\"label\":\"退款密码（管理密码）\",\"type\":\"input\",\"default\":\"\",\"help\":\"如不需要退款功能可留空\"},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"input\",\"default\":\"1\",\"help\":\"微信：1聚合支付 2H5预下单\"}"
             "]' WHERE plugin_code='huolian'"},
            {271, "jlpay builtin plugin",
             "UPDATE plugin_store SET display_name='嘉联支付',"
             "description=' jlpay 插件分析：嘉联OpenAPI使用SM3WithSM2WithDer签名、SM2-Hex密钥；当前C++项目未提供SM2实现，插件已注册但交易请求会明确返回不支持提示',"
             "vendor='嘉联支付',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,bank',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"应用appid\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appsecret\",\"label\":\"商户私钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"SM2-Hex格式\"},"
             "{\"key\":\"appkey\",\"label\":\"嘉联公钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"SM2-Hex格式\"},"
             "{\"key\":\"mch_id\",\"label\":\"商户号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"term_no\",\"label\":\"终端号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appswitch\",\"label\":\"环境选择\",\"type\":\"select\",\"default\":\"0\","
             "\"options\":[{\"value\":\"0\",\"label\":\"生产环境\"},{\"value\":\"1\",\"label\":\"测试环境\"}]},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"input\",\"default\":\"1\",\"help\":\"支付宝：1扫码 2JS；微信：1聚合扫码 2公众号/小程序；银行卡：1扫码 2JS\"}"
             "]' WHERE plugin_code='jlpay'"},
            {272, "jdpay builtin plugin",
             "UPDATE plugin_store SET display_name='京东支付',"
             "description=' jdpay 插件：RSA私钥加密签名+3DES-EDE3字段加密，表单跳转支付、XML回调验签、XML退款',"
             "vendor='京东支付',plugin_type=1,installed=0,state=1,"
             "supported_ways='jdpay',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"商户号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"商户DES密钥\",\"type\":\"input\",\"default\":\"\",\"help\":\"Base64编码的3DES密钥(24字节)\"},"
             "{\"key\":\"appsecret\",\"label\":\"商户RSA私钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"PEM格式，用于签名和加密\"},"
             "{\"key\":\"appmchid_pubkey\",\"label\":\"京东公钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"PEM格式，用于验签和解密\"}"
             "]' WHERE plugin_code='jdpay'"},
            {273, "kayixin builtin plugin",
             "UPDATE plugin_store SET display_name='卡易信(钱多多分账)',"
             "description=' kayixin 插件：仿支付宝接口，自定义网关域名，MD5签名(ksort去空值/sign/sign_type，拼接k=v&追加key)，表单跳转支付、回调验签',"
             "vendor='卡易信',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay',"
             "params_schema='["
             "{\"key\":\"appurl\",\"label\":\"接口域名\",\"type\":\"input\",\"default\":\"\",\"help\":\"如 http://trade.kayixin.com\"},"
             "{\"key\":\"appid\",\"label\":\"商户号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"商户密钥\",\"type\":\"input\",\"default\":\"\"}"
             "]' WHERE plugin_code='kayixin'"},
            {274, "kuaiqian builtin plugin",
             "UPDATE plugin_store SET display_name='快钱支付',"
             "description=' kuaiqian 插件：人民币网关RSA-SHA256签名+当面付PKCS7/CMS加签加密(待实现)，支持支付宝/微信/银行卡',"
             "vendor='快钱',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,bank',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"快钱账户号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"商户证书密码\",\"type\":\"input\",\"default\":\"\",\"help\":\"PFX私钥密码\"},"
             "{\"key\":\"appsecret\",\"label\":\"SSL客户端证书密码\",\"type\":\"input\",\"default\":\"\",\"help\":\"SSL双向证书密码(当面付需要)\"},"
             "{\"key\":\"appmchid\",\"label\":\"服务商-快钱子账户号\",\"type\":\"input\",\"default\":\"\",\"help\":\"仅服务商需要\"},"
             "{\"key\":\"merchant_id\",\"label\":\"当面付-商户号\",\"type\":\"input\",\"default\":\"\",\"help\":\"仅当面付需要\"},"
             "{\"key\":\"terminal_id\",\"label\":\"当面付-终端号\",\"type\":\"input\",\"default\":\"\",\"help\":\"仅当面付需要\"},"
             "{\"key\":\"own_channel\",\"label\":\"是否自有渠道\",\"type\":\"select\",\"default\":\"0\","
             "\"options\":[{\"value\":\"0\",\"label\":\"否\"},{\"value\":\"1\",\"label\":\"是\"}]},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"select\",\"default\":\"1\","
             "\"options\":[{\"value\":\"1\",\"label\":\"H5支付\"},{\"value\":\"2\",\"label\":\"当面付\"}]},"
             "{\"key\":\"merchant_pem\",\"label\":\"商户RSA私钥(PEM)\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"从PFX导出的PEM格式私钥\"},"
             "{\"key\":\"kuaiqian_pubkey\",\"label\":\"快钱公钥(PEM)\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"cert.cer导出的PEM格式公钥\"}"
             "]' WHERE plugin_code='kuaiqian'"},
            {275, "lakala builtin plugin",
             "UPDATE plugin_store SET display_name='拉卡拉',"
             "description=' lakala 插件：LKLAPI-SHA256withRSA签名(JSON Authorization头)，聚合扫码/收银台/被扫/JSAPI，支持支付宝/微信/云闪付',"
             "vendor='拉卡拉',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,bank',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"APPID\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appmchid\",\"label\":\"商户号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"终端号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appselect\",\"label\":\"接口类型\",\"type\":\"select\",\"default\":\"0\","
             "\"options\":[{\"value\":\"0\",\"label\":\"聚合扫码\"},{\"value\":\"1\",\"label\":\"聚合收银台\"}]},"
             "{\"key\":\"appswitch\",\"label\":\"环境选择\",\"type\":\"select\",\"default\":\"0\","
             "\"options\":[{\"value\":\"0\",\"label\":\"生产环境\"},{\"value\":\"1\",\"label\":\"测试环境\"}]},"
             "{\"key\":\"serial_no\",\"label\":\"商户证书序列号\",\"type\":\"input\",\"default\":\"\",\"help\":\"从api_cert.cer提取的序列号\"},"
             "{\"key\":\"merchant_pem\",\"label\":\"商户RSA私钥(PEM)\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"api_private_key.pem内容\"},"
             "{\"key\":\"lakala_pubkey\",\"label\":\"拉卡拉平台公钥(PEM)\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"lkl-apigw-v1.cer导出的PEM公钥\"}"
             "]' WHERE plugin_code='lakala'"},
            {276, "ltzf builtin plugin",
             "UPDATE plugin_store SET display_name='蓝兔支付',"
             "description=' ltzf 插件：MD5大写签名(ksort指定字段拼接k=v&追加key=)，JSON API，支持支付宝/微信扫码/H5/公众号',"
             "vendor='蓝兔支付',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"商户号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"商户密钥\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"select\",\"default\":\"1\","
             "\"options\":[{\"value\":\"1\",\"label\":\"扫码支付\"},{\"value\":\"2\",\"label\":\"H5支付\"},{\"value\":\"3\",\"label\":\"公众号支付\"}]}"
             "]' WHERE plugin_code='ltzf'"},
            {277, "passpay builtin plugin",
             "UPDATE plugin_store SET display_name='精秀支付',"
             "description=' passpay 插件：RSA-SHA256签名(ksort去空值/sign拼接k=v&)，JSON API(pay.order/create, pay.order/refund)，支持支付宝/微信/QQ/云闪付',"
             "vendor='精秀',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,qqpay,bank',"
             "params_schema='["
             "{\"key\":\"appurl\",\"label\":\"API接口地址\",\"type\":\"input\",\"default\":\"\",\"help\":\"以http://或https://开头，以/结尾\"},"
             "{\"key\":\"appid\",\"label\":\"商户编号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"商户私钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"RSA私钥(Base64编码)\"},"
             "{\"key\":\"appsecret\",\"label\":\"平台公钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"RSA公钥(Base64编码)\"},"
             "{\"key\":\"appmchid\",\"label\":\"通道ID\",\"type\":\"input\",\"default\":\"\",\"help\":\"不填写将进行子商户号轮训\"},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"select\",\"default\":\"1\","
             "\"options\":[{\"value\":\"1\",\"label\":\"扫码\"},{\"value\":\"2\",\"label\":\"PC/公众号\"},{\"value\":\"3\",\"label\":\"H5\"},{\"value\":\"4\",\"label\":\"小程序\"}]}"
             "]' WHERE plugin_code='passpay'"},
            {278, "paypal builtin plugin",
             "UPDATE plugin_store SET display_name='PayPal',"
             "description=' paypal 插件：PayPal V2 Orders API，OAuth2 Bearer Token认证，支持创建订单/捕获/退款/Webhook验签，多币种+汇率转换',"
             "vendor='PayPal',plugin_type=1,installed=0,state=1,"
             "supported_ways='paypal',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"ClientId\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"ClientSecret\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appswitch\",\"label\":\"模式选择\",\"type\":\"select\",\"default\":\"0\","
             "\"options\":[{\"value\":\"0\",\"label\":\"线上模式\"},{\"value\":\"1\",\"label\":\"沙盒模式\"}]},"
             "{\"key\":\"appsecret\",\"label\":\"Webhook ID\",\"type\":\"input\",\"default\":\"\",\"help\":\"PayPal Webhook ID，用于webhook验签\"},"
             "{\"key\":\"currency_code\",\"label\":\"结算货币\",\"type\":\"select\",\"default\":\"USD\","
             "\"options\":[{\"value\":\"USD\",\"label\":\"USD\"},{\"value\":\"EUR\",\"label\":\"EUR\"},{\"value\":\"GBP\",\"label\":\"GBP\"},{\"value\":\"CAD\",\"label\":\"CAD\"},{\"value\":\"AUD\",\"label\":\"AUD\"},{\"value\":\"JPY\",\"label\":\"JPY\"},{\"value\":\"HKD\",\"label\":\"HKD\"},{\"value\":\"CNY\",\"label\":\"CNY\"},{\"value\":\"SGD\",\"label\":\"SGD\"}]},"
             "{\"key\":\"currency_rate\",\"label\":\"货币汇率\",\"type\":\"input\",\"default\":\"1\",\"help\":\"1元人民币兑换目标货币的比率\"}"
             "]' WHERE plugin_code='paypal'"},
            {279, "qqpay builtin plugin",
             "UPDATE plugin_store SET display_name='QQ钱包官方支付',"
             "description=' qqpay 插件：QQ钱包官方XML API(qpay.qq.com)，MD5大写签名(ksort拼接k=v&追加key=)，扫码/JSAPI/被扫/退款/关闭/转账',"
             "vendor='QQ钱包',plugin_type=1,installed=0,state=1,"
             "supported_ways='qqpay',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"QQ钱包商户号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"QQ钱包API密钥\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appurl\",\"label\":\"操作员账号\",\"type\":\"input\",\"default\":\"\",\"help\":\"仅退款/企业付款时需要\"},"
             "{\"key\":\"appmchid\",\"label\":\"操作员密码\",\"type\":\"input\",\"default\":\"\",\"help\":\"仅退款/企业付款时需要\"},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"select\",\"default\":\"1\","
             "\"options\":[{\"value\":\"1\",\"label\":\"扫码支付(含H5)\"},{\"value\":\"2\",\"label\":\"公众号支付\"}]},"
             "{\"key\":\"merchant_pem\",\"label\":\"商户证书私钥(PEM)\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"退款需要SSL双向证书\"},"
             "{\"key\":\"merchant_cert\",\"label\":\"商户证书(PEM)\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"退款需要SSL双向证书\"}"
             "]' WHERE plugin_code='qqpay'"},
            {280, "sandpay builtin plugin",
             "UPDATE plugin_store SET display_name='杉德支付',"
             "description=' sandpay 插件：RSA-SHA256签名+AES-128-ECB加密+RSA公钥加密AES密钥，V4 JSON API，支持支付宝/微信/银联扫码/JSAPI/快捷支付/退款/转账',"
             "vendor='杉德',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,bank',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"商户编号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"私钥证书密码\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appswitch\",\"label\":\"环境选择\",\"type\":\"select\",\"default\":\"0\","
             "\"options\":[{\"value\":\"0\",\"label\":\"生产环境\"},{\"value\":\"1\",\"label\":\"测试环境\"}]},"
             "{\"key\":\"product\",\"label\":\"市场产品\",\"type\":\"select\",\"default\":\"QZF\","
             "\"options\":[{\"value\":\"QZF\",\"label\":\"标准线上收款\"},{\"value\":\"CSDB\",\"label\":\"企业杉德宝\"}]},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"select\",\"default\":\"1\","
             "\"options\":[{\"value\":\"1\",\"label\":\"扫码\"},{\"value\":\"2\",\"label\":\"JSAPI/公众号\"},{\"value\":\"3\",\"label\":\"快捷支付\"}]},"
             "{\"key\":\"merchant_pem\",\"label\":\"商户RSA私钥(PEM)\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"从PFX导出的PEM格式私钥\"},"
             "{\"key\":\"sand_pubkey\",\"label\":\"杉德公钥(PEM)\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"从sand.cer导出的PEM格式公钥\"}"
             "]' WHERE plugin_code='sandpay'"},
            {281, "shengpay builtin plugin",
             "UPDATE plugin_store SET display_name='盛付通',"
             "description=' shengpay 插件：RSA-SHA1签名(ksort去空值/sign拼接k=v&)，JSON API，支持支付宝/微信/银联扫码/JSAPI/H5/退款/转账/分账',"
             "vendor='盛付通',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,bank',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"商户号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"商户私钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"RSA私钥(Base64编码)\"},"
             "{\"key\":\"appsecret\",\"label\":\"盛付通公钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"RSA公钥(Base64编码)\"},"
             "{\"key\":\"appswitch\",\"label\":\"收单接口类型\",\"type\":\"select\",\"default\":\"0\","
             "\"options\":[{\"value\":\"0\",\"label\":\"线上\"},{\"value\":\"1\",\"label\":\"线下\"}]},"
             "{\"key\":\"appmchid\",\"label\":\"子商户号\",\"type\":\"input\",\"default\":\"\",\"help\":\"非代理商户可留空\"},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"select\",\"default\":\"1\","
             "\"options\":[{\"value\":\"1\",\"label\":\"扫码\"},{\"value\":\"2\",\"label\":\"PC网站\"},{\"value\":\"3\",\"label\":\"H5/手机\"},{\"value\":\"4\",\"label\":\"JSAPI/服务窗\"},{\"value\":\"5\",\"label\":\"聚合码\"}]},"
             "{\"key\":\"aeskey\",\"label\":\"AES加密密钥\",\"type\":\"input\",\"default\":\"\",\"help\":\"用于投诉事件回调解密\"}"
             "]' WHERE plugin_code='shengpay'"},
            {282, "leshua builtin plugin",
             "UPDATE plugin_store SET display_name='乐刷聚合支付',"
             "description=' leshua 插件：MD5大写签名(ksort跳sign/error_code拼接k=v&追加key=)，form POST+XML响应，支持支付宝/微信/银联扫码/JSAPI/退款',"
             "vendor='乐刷',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,bank',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"商户号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"交易密钥\",\"type\":\"input\",\"default\":\"\",\"help\":\"下单/退款签名用\"},"
             "{\"key\":\"appsecret\",\"label\":\"异步通知密钥\",\"type\":\"input\",\"default\":\"\",\"help\":\"回调验签用\"},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"select\",\"default\":\"1\","
             "\"options\":[{\"value\":\"1\",\"label\":\"扫码\"},{\"value\":\"2\",\"label\":\"JSAPI/公众号\"}]}"
             "]' WHERE plugin_code='leshua'"},
            {283, "lzyzf builtin plugin",
             "UPDATE plugin_store SET display_name='浪子易支付',"
             "description=' lzyzf 插件：易支付兼容协议，MD5签名(ksort跳sign/sign_type/空值拼接k=v&去尾&直接追加key)，submit.php页面跳转/mapi.php JSON API/回调GET验签/退款api.php',"
             "vendor='浪子',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,qqpay,wxpay,bank,jdpay',"
             "params_schema='["
             "{\"key\":\"appurl\",\"label\":\"接口地址\",\"type\":\"input\",\"default\":\"\",\"help\":\"必须以http://或https://开头，以/结尾\"},"
             "{\"key\":\"appid\",\"label\":\"商户ID\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"商户密钥\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appswitch\",\"label\":\"是否使用mapi接口\",\"type\":\"select\",\"default\":\"0\","
             "\"options\":[{\"value\":\"0\",\"label\":\"否(页面跳转)\"},{\"value\":\"1\",\"label\":\"是(API接口)\"}]}"
             "]' WHERE plugin_code='lzyzf'"},
            {284, "stripe builtin plugin",
             "UPDATE plugin_store SET display_name='Stripe',"
             "description=' stripe 插件：Bearer Token认证(sk_live_)，Checkout Session收银台/PaymentIntent直接支付，Webhook HMAC-SHA256验签(whsec_)，支持支付宝/微信/PayPal/银联/退款',"
             "vendor='Stripe',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,bank,paypal',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"API密钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"sk_live_开头的密钥\"},"
             "{\"key\":\"appkey\",\"label\":\"Webhook密钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"whsec_开头的密钥\"},"
             "{\"key\":\"appswitch\",\"label\":\"支付模式\",\"type\":\"select\",\"default\":\"0\","
             "\"options\":[{\"value\":\"0\",\"label\":\"跳转收银台\"},{\"value\":\"1\",\"label\":\"直接支付(仅限支付宝/微信)\"}]},"
             "{\"key\":\"currency_code\",\"label\":\"结算货币\",\"type\":\"select\",\"default\":\"CNY\","
             "\"options\":[{\"value\":\"CNY\",\"label\":\"人民币(CNY)\"},{\"value\":\"HKD\",\"label\":\"港币(HKD)\"},{\"value\":\"EUR\",\"label\":\"欧元(EUR)\"},{\"value\":\"USD\",\"label\":\"美元(USD)\"},{\"value\":\"JPY\",\"label\":\"日元(JPY)\"},{\"value\":\"GBP\",\"label\":\"英镑(GBP)\"},{\"value\":\"SGD\",\"label\":\"新加坡元(SGD)\"}]},"
             "{\"key\":\"currency_rate\",\"label\":\"货币汇率\",\"type\":\"input\",\"default\":\"1\",\"help\":\"例如1元人民币兑换0.137USD则填0.137\"}"
             "]' WHERE plugin_code='stripe'"},
            {285, "suixingpay builtin plugin",
             "UPDATE plugin_store SET display_name='随行付',"
             "description=' suixingpay 插件：RSA-SHA1签名(ksort跳sign/null/空拼接k=v&数组JSON编码)，JSON API，扫码/JSAPI/小程序/退款，网关openapi.tianquetech.com',"
             "vendor='随行付',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,bank',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"机构编号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"平台公钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"RSA公钥(Base64编码)\"},"
             "{\"key\":\"appsecret\",\"label\":\"商户私钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"RSA私钥(Base64编码)\"},"
             "{\"key\":\"appmchid\",\"label\":\"商户编号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"select\",\"default\":\"1\","
             "\"options\":[{\"value\":\"1\",\"label\":\"扫码\"},{\"value\":\"2\",\"label\":\"JSAPI/公众号\"}]}"
             "]' WHERE plugin_code='suixingpay'"},
            {286, "swiftpass builtin plugin",
             "UPDATE plugin_store SET display_name='威富通RSA',"
             "description=' swiftpass 插件：RSA_1_256签名(ksort跳sign/空值拼接k=v&去尾&)，XML POST/响应，扫码/JSAPI/H5/退款，网关pay.swiftpass.cn可自定义',"
             "vendor='威富通',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,qqpay,bank,jdpay',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"商户号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"平台RSA公钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"RSA公钥(Base64编码)\"},"
             "{\"key\":\"appsecret\",\"label\":\"商户RSA私钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"RSA私钥(Base64编码)\"},"
             "{\"key\":\"appurl\",\"label\":\"自定义网关URL\",\"type\":\"input\",\"default\":\"\",\"help\":\"可不填,默认https://pay.swiftpass.cn/pay/gateway\"},"
             "{\"key\":\"appswitch\",\"label\":\"微信是否支持H5\",\"type\":\"select\",\"default\":\"0\","
             "\"options\":[{\"value\":\"0\",\"label\":\"否\"},{\"value\":\"1\",\"label\":\"是\"}]},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"select\",\"default\":\"1\","
             "\"options\":[{\"value\":\"1\",\"label\":\"扫码\"},{\"value\":\"2\",\"label\":\"JSAPI/公众号\"},{\"value\":\"3\",\"label\":\"H5\"}]}"
             "]' WHERE plugin_code='swiftpass'"},
            {287, "swiftpass2 builtin plugin",
             "UPDATE plugin_store SET display_name='威富通MD5',"
             "description=' swiftpass2 插件：MD5大写签名(ksort跳sign/空值拼接k=v&去尾&追加&key=)，XML POST/响应，扫码/JSAPI/H5/退款，网关pay.swiftpass.cn可自定义',"
             "vendor='威富通',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,qqpay,bank,jdpay',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"商户号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"商户密钥\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appurl\",\"label\":\"自定义网关URL\",\"type\":\"input\",\"default\":\"\",\"help\":\"可不填,默认https://pay.swiftpass.cn/pay/gateway\"},"
             "{\"key\":\"appswitch\",\"label\":\"微信是否支持H5\",\"type\":\"select\",\"default\":\"0\","
             "\"options\":[{\"value\":\"0\",\"label\":\"否\"},{\"value\":\"1\",\"label\":\"是\"}]},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"select\",\"default\":\"1\","
             "\"options\":[{\"value\":\"1\",\"label\":\"扫码\"},{\"value\":\"2\",\"label\":\"JSAPI/公众号\"},{\"value\":\"3\",\"label\":\"H5\"}]}"
             "]' WHERE plugin_code='swiftpass2'"},
            {288, "umfpay builtin plugin",
             "UPDATE plugin_store SET display_name='联动优势',"
             "description=' umfpay 插件：RSA-SHA1签名(ksort跳sign/sign_type/空值拼接k=v&去尾&)，form POST，HTML META响应解析，扫码/公众号/退款，网关pay.soopay.net',"
             "vendor='联动优势',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,bank',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"商户编号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"平台公钥(cert.pem)\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"PEM格式平台公钥\"},"
             "{\"key\":\"appsecret\",\"label\":\"商户私钥(key.pem)\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"PEM格式商户私钥\"},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"select\",\"default\":\"1\","
             "\"options\":[{\"value\":\"1\",\"label\":\"扫码\"},{\"value\":\"2\",\"label\":\"公众号\"}]}"
             "]' WHERE plugin_code='umfpay'"},
            {289, "unionpay_gateway builtin plugin",
             "UPDATE plugin_store SET display_name='银联前置',"
             "description=' unionpay 插件：MD5大写签名(威富通协议)，XML POST/响应，扫码/JSAPI/H5/支付宝服务窗/云闪付JS/退款，网关qra.95516.com可自定义',"
             "vendor='银联',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,qqpay,bank,jdpay',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"商户号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"商户密钥\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appurl\",\"label\":\"自定义网关URL\",\"type\":\"input\",\"default\":\"\",\"help\":\"可不填,默认https://qra.95516.com/pay/gateway\"},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"select\",\"default\":\"1\","
             "\"options\":[{\"value\":\"1\",\"label\":\"扫码\"},{\"value\":\"2\",\"label\":\"JSAPI/公众号\"},{\"value\":\"3\",\"label\":\"H5\"},{\"value\":\"4\",\"label\":\"支付宝服务窗\"},{\"value\":\"5\",\"label\":\"云闪付JS\"}]}"
             "]' WHERE plugin_code='unionpay_gateway'"},
            {290, "usdtpro builtin plugin",
             "UPDATE plugin_store SET display_name='USDTPRO V2',"
             "description=' usdtpro 插件：USDT TRC20虚拟货币支付，展示收款地址+精确金额(5位小数防重复)，Tronscan API轮询匹配到账，不支持退款',"
             "vendor='虚拟货币易支付',plugin_type=1,installed=0,state=1,"
             "supported_ways='usdt,qqpay',"
             "params_schema='["
             "{\"key\":\"address\",\"label\":\"USDT TRC20地址\",\"type\":\"input\",\"default\":\"\",\"help\":\"TRC20收款地址\"},"
             "{\"key\":\"rate\",\"label\":\"汇率 $1 = ￥?\",\"type\":\"input\",\"default\":\"7.2\",\"help\":\"1 USDT兑换多少人民币\"},"
             "{\"key\":\"timeout\",\"label\":\"超时时间(秒)\",\"type\":\"input\",\"default\":\"300\",\"help\":\"订单超时时间\"}"
             "]' WHERE plugin_code='usdtpro'"},
            {291, "xorpay builtin plugin",
             "UPDATE plugin_store SET display_name='XorPay',"
             "description=' xorpay 插件：MD5拼接签名(非ksort，直接拼接字段+appkey)，form POST，JSON响应，扫码/收银台JSAPI/退款，API xorpay.com',"
             "vendor='XorPay',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"AppId\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"AppSecret\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"select\",\"default\":\"1\","
             "\"options\":[{\"value\":\"1\",\"label\":\"扫码\"},{\"value\":\"2\",\"label\":\"收银台JSAPI\"}]}"
             "]' WHERE plugin_code='xorpay'"},
            {292, "xsy builtin plugin",
             "UPDATE plugin_store SET display_name='新生易',"
             "description=' xsy 插件：RSA-SHA1签名(ksort拼接k=v&，reqData/respData JSON编码后拼接)，JSON POST，扫码/JSAPI/被扫/退款，网关gateway-hpx.hnapay.com',"
             "vendor='新生易',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,bank',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"机构代码\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"平台公钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"RSA公钥(Base64编码)\"},"
             "{\"key\":\"appsecret\",\"label\":\"商户私钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"RSA私钥(Base64编码)\"},"
             "{\"key\":\"appmchid\",\"label\":\"商户编号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appswitch\",\"label\":\"环境选择\",\"type\":\"select\",\"default\":\"0\","
             "\"options\":[{\"value\":\"0\",\"label\":\"生产环境\"},{\"value\":\"1\",\"label\":\"测试环境\"}]},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"select\",\"default\":\"1\","
             "\"options\":[{\"value\":\"1\",\"label\":\"扫码\"},{\"value\":\"2\",\"label\":\"JSAPI\"}]}"
             "]' WHERE plugin_code='xsy'"},
            {293, "xunhupay builtin plugin",
             "UPDATE plugin_store SET display_name='虎皮椒支付',"
             "description='xunhupay 插件：MD5签名(ksort跳hash/空值拼接k=v&去尾&直接追加key)，JSON POST/响应，扫码/微信H5/退款，网关api.xunhupay.com可自定义',"
             "vendor='虎皮椒',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"商户ID\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"API密钥\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appurl\",\"label\":\"网关地址\",\"type\":\"input\",\"default\":\"\",\"help\":\"不填写默认https://api.xunhupay.com/payment/do.html\"}"
             "]' WHERE plugin_code='xunhupay'"},
            {294, "yeepay builtin plugin",
             "UPDATE plugin_store SET display_name='易宝支付',"
             "description='按彩虹易 yeepay 插件：YOP认证协议(RSA-SHA256签名Authorization header)，AES-128-ECB回调解密，扫码/JSAPI/托管H5/APP/退款，网关openapi.yeepay.com',"
             "vendor='易宝支付',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,bank',"
             "params_schema='["
             "{\"key\":\"appkey\",\"label\":\"应用标识\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appsecret\",\"label\":\"商户私钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"RSA私钥(Base64编码)\"},"
             "{\"key\":\"appid\",\"label\":\"发起方商户编号\",\"type\":\"input\",\"default\":\"\",\"help\":\"标准商户填商编；平台商填平台商商编\"},"
             "{\"key\":\"appmchid\",\"label\":\"收款商户编号\",\"type\":\"input\",\"default\":\"\",\"help\":\"留空则与发起方商户编号一致\"},"
             "{\"key\":\"appswitch\",\"label\":\"支付场景\",\"type\":\"select\",\"default\":\"0\","
             "\"options\":[{\"value\":\"0\",\"label\":\"线上\"},{\"value\":\"1\",\"label\":\"线下\"}]},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"select\",\"default\":\"1\","
             "\"options\":[{\"value\":\"1\",\"label\":\"扫码\"},{\"value\":\"2\",\"label\":\"JSAPI\"},{\"value\":\"3\",\"label\":\"托管H5\"}]}"
             "]' WHERE plugin_code='yeepay'"},
            {295, "yinyingtong builtin plugin",
             "UPDATE plugin_store SET display_name='银盈通支付',"
             "description='按彩虹易 yinyingtong 插件：MD5签名(仅指定字段)，JSON POST，DES-ECB回调解密，扫码/JSAPI/小程序/快捷/退款，网关gc-gw.gomepay.com',"
             "vendor='银盈通',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,bank',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"应用ID\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"应用KEY\",\"type\":\"input\",\"default\":\"\",\"help\":\"同时是私钥证书密码\"},"
             "{\"key\":\"productkey\",\"label\":\"产品密钥\",\"type\":\"input\",\"default\":\"\",\"help\":\"用于支付回调数据解密\"},"
             "{\"key\":\"appmchid\",\"label\":\"交易商户企业号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"trade_platform_no\",\"label\":\"平台商企业号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"channel_merch_no\",\"label\":\"渠道商户号\",\"type\":\"input\",\"default\":\"\",\"help\":\"多个用逗号分隔\"},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"select\",\"default\":\"1\","
             "\"options\":[{\"value\":\"1\",\"label\":\"扫码\"},{\"value\":\"2\",\"label\":\"小程序\"},{\"value\":\"3\",\"label\":\"自有公众号\"}]}"
             "]' WHERE plugin_code='yinyingtong'"},
            {296, "ysepay builtin plugin",
             "UPDATE plugin_store SET display_name='银盛支付',"
             "description='按彩虹易 ysepay 插件：RSA证书签名(ksort拼接)，form POST，扫码/H5/JSAPI/退款，网关qrcode.ysepay.com/openapi.ysepay.com',"
             "vendor='银盛支付',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,qqpay,wxpay,bank',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"服务商商户号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"私钥证书密码\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appmchid\",\"label\":\"收款商户号\",\"type\":\"input\",\"default\":\"\",\"help\":\"不填写则和服务商商户号相同\"},"
             "{\"key\":\"appurl\",\"label\":\"业务代码\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appsecret\",\"label\":\"商户私钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"RSA私钥(PKCS#8 PEM格式)\"},"
             "{\"key\":\"productkey\",\"label\":\"平台公钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"RSA公钥(PEM格式)\"},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"select\",\"default\":\"1\","
             "\"options\":[{\"value\":\"1\",\"label\":\"扫码\"},{\"value\":\"2\",\"label\":\"H5/公众号\"},{\"value\":\"3\",\"label\":\"生活号\"}]}"
             "]' WHERE plugin_code='ysepay'"},
            {297, "yseqt builtin plugin",
             "UPDATE plugin_store SET display_name='银盛e企通',"
             "description='按彩虹易 yseqt 插件：RSA证书签名(ksort拼接)，form POST，扫码/JSAPI/小程序/转账/退款，网关eqt.ysepay.com',"
             "vendor='银盛',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,bank',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"服务商商户号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"私钥证书密码\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appmchid\",\"label\":\"收款商户号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appsecret\",\"label\":\"商户私钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"RSA私钥(PKCS#8 PEM格式)\"},"
             "{\"key\":\"productkey\",\"label\":\"平台公钥\",\"type\":\"textarea\",\"default\":\"\",\"help\":\"RSA公钥(PEM格式)\"},"
             "{\"key\":\"apptype\",\"label\":\"支付方式\",\"type\":\"select\",\"default\":\"1\","
             "\"options\":[{\"value\":\"1\",\"label\":\"扫码\"},{\"value\":\"2\",\"label\":\"小程序H5\"},{\"value\":\"3\",\"label\":\"JS支付\"}]}"
             "]' WHERE plugin_code='yseqt'"},
            {298, "zhangyishou builtin plugin",
             "UPDATE plugin_store SET display_name='掌易收聚合支付',"
             "description='按彩虹易 zhangyishou 插件：MD5拼接签名(直接拼接值)，JSON POST，扫码/转账/余额查询/退款，网关apipay.zhangyishou.com',"
             "vendor='掌易收',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,qqpay,wxpay,bank',"
             "params_schema='["
             "{\"key\":\"appid\",\"label\":\"登录账号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"商户密钥\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appurl\",\"label\":\"商户编号\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appmchid\",\"label\":\"通道ID\",\"type\":\"input\",\"default\":\"\",\"help\":\"多个ID用|分隔\"}"
             "]' WHERE plugin_code='zhangyishou'"},
            {300, "agpayplus builtin plugin",
             "INSERT OR IGNORE INTO plugin_store(plugin_code,display_name,description,vendor,version,icon,plugin_type,supported_ways,installed,state,has_callback,has_refund) VALUES"
             "('agpayplus','AgPay Plus聚合支付','agpayplus 插件：协议同Jeepay/MD5签名/reqTime秒级/回调验签/退款','AgPay','1.0.0','💳',1,'wxpay,alipay,bank',0,1,1,1);"
             "UPDATE plugin_store SET display_name='AgPay Plus聚合支付',"
             "description='agpayplus 插件：协议同Jeepay/MD5签名/reqTime秒级/回调验签/退款',"
             "vendor='AgPay',plugin_type=1,installed=0,state=1,"
             "supported_ways='alipay,wxpay,bank',"
             "params_schema='["
             "{\"key\":\"appurl\",\"label\":\"接口地址\",\"type\":\"input\",\"default\":\"\",\"help\":\"必须以 http:// 或 https:// 开头，以 / 结尾\"},"
             "{\"key\":\"appmchid\",\"label\":\"商户号 mchNo\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appid\",\"label\":\"应用 AppId\",\"type\":\"input\",\"default\":\"\"},"
             "{\"key\":\"appkey\",\"label\":\"私钥 ApiKey\",\"type\":\"textarea\",\"default\":\"\"},"
             "{\"key\":\"apptype\",\"label\":\"支付模式\",\"type\":\"input\",\"default\":\"1\",\"help\":\"1扫码 2H5/PC 3JSAPI/WAP 4小程序 5聚合扫码 6WEB收银台 7APP，逗号分隔\"}"
             "]' WHERE plugin_code='agpayplus'"},
            {299, "default wepay channels + enable qrcodes",
             "INSERT OR IGNORE INTO pay_channel(channel_code,channel_name,pay_type,plugin,rate,state,params_json,sort_order) VALUES"
             "('wepay_wx','WePay微信通道','wxpay','wepay','0.6',1,'{}',10),"
             "('wepay_ali','WePay支付宝通道','alipay','wepay','0.6',1,'{}',11),"
             "('wepay_qq','WePay QQ通道','qqpay','wepay','0.6',1,'{}',12);"
             "UPDATE pay_qrcode SET state=0 WHERE state!=0"},
            {300, "plugin_store add default_params",
             "ALTER TABLE plugin_store ADD COLUMN default_params TEXT NOT NULL DEFAULT '{}'"},

            // ══════════════════════════════════════════════════════
            // v310~v313: AI 智能助手 + AI 安全网关
            // ══════════════════════════════════════════════════════
            {310, "create ai_chat_session",
             "CREATE TABLE IF NOT EXISTS ai_chat_session("
             "session_id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "user_name TEXT NOT NULL DEFAULT '',"
             "title TEXT NOT NULL DEFAULT '新对话',"
             "model_name TEXT NOT NULL DEFAULT '讯飞星火',"
             "create_time TEXT NOT NULL DEFAULT (datetime('now')),"
             "update_time TEXT NOT NULL DEFAULT (datetime('now')))"},
            {311, "create ai_chat_message",
             "CREATE TABLE IF NOT EXISTS ai_chat_message("
             "msg_id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "session_id INTEGER NOT NULL,"
             "role TEXT NOT NULL DEFAULT 'user',"
             "content TEXT NOT NULL DEFAULT '',"
             "create_time TEXT NOT NULL DEFAULT (datetime('now')))"},
            {312, "idx ai_chat_message session",
             "CREATE INDEX IF NOT EXISTS idx_ai_msg_session ON ai_chat_message(session_id)"},
            {313, "create ai_threat_log",
             "CREATE TABLE IF NOT EXISTS ai_threat_log("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "ip TEXT NOT NULL DEFAULT '',"
             "path TEXT NOT NULL DEFAULT '',"
             "method TEXT NOT NULL DEFAULT '',"
             "threat_type TEXT NOT NULL DEFAULT '',"
             "score INTEGER NOT NULL DEFAULT 0,"
             "action TEXT NOT NULL DEFAULT 'LOG',"
             "detail TEXT NOT NULL DEFAULT '',"
             "create_time TEXT NOT NULL DEFAULT (datetime('now')))"},

            // v320~v322: 自定义表单（参考若依风格）
            {320, "create custom_form",
             "CREATE TABLE IF NOT EXISTS custom_form("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "code TEXT NOT NULL UNIQUE,"                 // URL slug 唯一标识
             "title TEXT NOT NULL DEFAULT '',"
             "description TEXT NOT NULL DEFAULT '',"
             "form_fields TEXT NOT NULL DEFAULT '[]',"    // 字段列表 JSON（drawingList）
             "form_conf TEXT NOT NULL DEFAULT '{}',"      // 表单全局配置 JSON
             "state INTEGER NOT NULL DEFAULT 1,"          // 1启用 0禁用
             "require_login INTEGER NOT NULL DEFAULT 0,"  // 0匿名 1登录
             "created_by TEXT NOT NULL DEFAULT '',"
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "updated_at INTEGER NOT NULL DEFAULT 0)"},
            {321, "create custom_form_submission",
             "CREATE TABLE IF NOT EXISTS custom_form_submission("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "form_id INTEGER NOT NULL,"
             "form_code TEXT NOT NULL DEFAULT '',"
             "submitter TEXT NOT NULL DEFAULT '',"        // 用户名（登录时）
             "submitter_ip TEXT NOT NULL DEFAULT '',"
             "payload_json TEXT NOT NULL DEFAULT '{}',"
             "created_at INTEGER NOT NULL DEFAULT 0)"},
            {322, "idx custom_form_submission",
             "CREATE INDEX IF NOT EXISTS idx_cfs_form ON custom_form_submission(form_id, created_at)"},

            // ══════════════════════════════════════════════════════
            // webpay (CEF 浏览器抓包) 相关表
            // ══════════════════════════════════════════════════════
            {400, "create cef_bill_log (webpay 抓包原始账单落库, 用于 MQ 消费端离线时事后重放)",
             "CREATE TABLE IF NOT EXISTS cef_bill_log("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "captured_at INTEGER NOT NULL DEFAULT 0,"
             "source_url TEXT NOT NULL DEFAULT '',"
             "raw_body TEXT NOT NULL DEFAULT '',"
             "entries_cnt INTEGER NOT NULL DEFAULT 0,"
             "matched_oids TEXT NOT NULL DEFAULT '',"
             "created_at INTEGER NOT NULL DEFAULT 0)"},
            {401, "index cef_bill_log captured_at",
             "CREATE INDEX IF NOT EXISTS idx_cef_bill_captured_at ON cef_bill_log(captured_at)"},

            {402, "register cef_alipay_001 as a device (CEF 浏览器当云端设备, device_type=4)",
             // 用 0 占位 created_at/updated_at, 首次请求时 CefBillCtrl 的 UPDATE 会填真实值
             // 兼容 SQLite 和 PostgreSQL, 避免 strftime/NOW() 方言差异
             "INSERT OR IGNORE INTO device(device_no,device_name,device_type,state,"
             "created_at,updated_at) VALUES("
             "'cef_alipay_001','webpay-cef浏览器',4,1,0,0)"},

            // ══════════════════════════════════════════════════════
            // v410~v412: 网页流水监听 (receipt_watcher)
            // ══════════════════════════════════════════════════════
            {410, "create receipt_flow_log",
             "CREATE TABLE IF NOT EXISTS receipt_flow_log("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "flow_no TEXT NOT NULL UNIQUE,"
             "amount TEXT NOT NULL DEFAULT '0.00',"
             "pay_type TEXT NOT NULL DEFAULT '',"
             "raw_json TEXT NOT NULL DEFAULT '',"
             "status INTEGER NOT NULL DEFAULT 0,"
             "matched_order TEXT NOT NULL DEFAULT '',"
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "updated_at INTEGER NOT NULL DEFAULT 0)"},
            {411, "index receipt_flow_log status",
             "CREATE INDEX IF NOT EXISTS idx_rfl_status ON receipt_flow_log(status, created_at)"},
            {412, "register sms_receipt plugin",
             "INSERT OR IGNORE INTO plugin_store("
             "plugin_code,display_name,description,vendor,version,icon,"
             "plugin_type,supported_ways,price,installed,state,created_at,bg_color,"
             "params_schema) VALUES("
             "'sms_receipt','手动确认免签','用户扫码付款后提交交易订单号确认收款，"
             "支持邮件通知管理员审核，无需挂机/API/Python','WePay','1.0.0','📱',1,"
             "'alipay,wxpay,qqpay',0,0,1,0,'#7c3aed',"
             "'["
             "{\"key\":\"qrcode_url\",\"label\":\"收款码内容\",\"type\":\"input\",\"default\":\"\",\"required\":true,\"help\":\"个人收款码URL\"},"
             "{\"key\":\"qrcode_image\",\"label\":\"收款码图片\",\"type\":\"image\",\"default\":\"\",\"help\":\"展示给用户的图片地址\"},"
             "{\"key\":\"amount_offset_max\",\"label\":\"最大偏移(分)\",\"type\":\"number\",\"default\":\"99\",\"help\":\"金额偏移0~N分\"},"
             "{\"key\":\"receipt_valid_seconds\",\"label\":\"有效期(秒)\",\"type\":\"number\",\"default\":\"300\"},"
             "{\"key\":\"auto_confirm\",\"label\":\"自动确认\",\"type\":\"select\",\"default\":\"1\","
             "\"options\":[{\"value\":\"1\",\"label\":\"是(用户提交立即确认)\"},{\"value\":\"0\",\"label\":\"否(发邮件管理员审核)\"}]},"
             "{\"key\":\"admin_email\",\"label\":\"审核邮箱\",\"type\":\"input\",\"default\":\"\",\"help\":\"管理员审核邮箱(审核模式必填)\"}"
             "]')"},
            {413, "register receipt_monitor plugin",
             "INSERT OR IGNORE INTO plugin_store("
             "plugin_code,display_name,description,vendor,version,icon,"
             "plugin_type,supported_ways,price,installed,state,created_at,bg_color,"
             "params_schema) VALUES("
             "'receipt_monitor','网页流水监听','配合 Python receipt_watcher 自动抓取支付宝/微信流水匹配订单，"
             "无需挂机手机，用户扫码付款即可自动确认','WePay','1.0.0','📡',1,"
             "'alipay,wxpay',0,0,1,0,'#059669',"
             "'["
             "{\"key\":\"qrcode_url\",\"label\":\"收款码内容\",\"type\":\"input\",\"default\":\"\",\"required\":true,\"help\":\"个人收款码扫码后的URL\"},"
             "{\"key\":\"qrcode_image\",\"label\":\"收款码图片\",\"type\":\"image\",\"default\":\"\",\"help\":\"展示给用户的图片地址\"},"
             "{\"key\":\"amount_offset_max\",\"label\":\"最大偏移(分)\",\"type\":\"number\",\"default\":\"99\",\"help\":\"金额偏移0~N分避免重复\"},"
             "{\"key\":\"receipt_valid_seconds\",\"label\":\"有效期(秒)\",\"type\":\"number\",\"default\":\"300\"}"
             "]')"},
            {415, "register bepusdt plugin",
             "INSERT OR IGNORE INTO plugin_store("
             "plugin_code,display_name,description,vendor,version,icon,"
             "plugin_type,supported_ways,price,installed,state,created_at,bg_color,"
             "params_schema) VALUES("
             "'bepusdt','BEpusdt加密货币','多链加密货币支付网关(USDT/ETH/BNB/USDC等10+链)，"
             "自带收银台和管理后台，底层区块扫描确认，汇率自动更新','BEpusdt','1.0.0','₿',1,"
             "'usdt',0,0,1,0,'#f7931a',"
             "'["
             "{\"key\":\"api_token\",\"label\":\"API Token\",\"type\":\"password\",\"default\":\"\",\"required\":true,\"help\":\"BEpusdt后台→系统管理→基本设置→API设置→对接令牌\"},"
             "{\"key\":\"trade_type\",\"label\":\"交易类型\",\"type\":\"select\",\"default\":\"usdt.trc20\",\"help\":\"默认币种网络\",\"options\":[\"usdt.trc20\",\"usdt.erc20\",\"usdt.polygon\",\"usdt.bsc\",\"usdc.polygon\",\"tron.trx\",\"ethereum.eth\",\"bsc.bnb\"]},"
             "{\"key\":\"fiat\",\"label\":\"法币类型\",\"type\":\"select\",\"default\":\"CNY\",\"options\":[\"CNY\",\"USD\",\"EUR\",\"GBP\",\"JPY\"]},"
             "{\"key\":\"timeout\",\"label\":\"超时时间(秒)\",\"type\":\"number\",\"default\":\"600\",\"help\":\"最低120秒\"}"
             "]')"},

            {416, "update qrcode_image type to image",
             "UPDATE plugin_store SET params_schema = REPLACE(params_schema, "
             "'\"type\":\"input\",\"default\":\"\",\"help\":\"展示给用户的图片地址\"', "
             "'\"type\":\"image\",\"default\":\"\",\"help\":\"展示给用户的图片地址\"') "
             "WHERE params_schema LIKE '%qrcode_image%'"},
            {414, "register alipay_email plugin",
             "INSERT OR IGNORE INTO plugin_store("
             "plugin_code,display_name,description,vendor,version,icon,"
             "plugin_type,supported_ways,price,installed,state,created_at,bg_color,"
             "params_schema) VALUES("
             "'alipay_email','邮件通知免签','通过 IMAP 轮询支付宝收款通知邮件自动匹配订单，"
             "无需登录支付宝后台、无需浏览器、无需API资质，极其稳定','WePay','1.0.0','📧',1,"
             "'alipay',0,0,1,0,'#2563eb',"
             "'["
             "{\"key\":\"qrcode_url\",\"label\":\"收款码内容\",\"type\":\"input\",\"default\":\"\",\"required\":true,\"help\":\"个人收款码URL\"},"
             "{\"key\":\"qrcode_image\",\"label\":\"收款码图片\",\"type\":\"image\",\"default\":\"\",\"help\":\"展示给用户的图片地址\"},"
             "{\"key\":\"amount_offset_max\",\"label\":\"最大偏移(分)\",\"type\":\"number\",\"default\":\"99\",\"help\":\"金额偏移0~N分\"},"
             "{\"key\":\"receipt_valid_seconds\",\"label\":\"有效期(秒)\",\"type\":\"number\",\"default\":\"300\"},"
             "{\"key\":\"email_imap_host\",\"label\":\"IMAP服务器\",\"type\":\"input\",\"default\":\"imap.qq.com\",\"required\":true,\"help\":\"如 imap.qq.com / imap.163.com / imap.gmail.com\"},"
             "{\"key\":\"email_imap_port\",\"label\":\"IMAP端口\",\"type\":\"number\",\"default\":\"993\"},"
             "{\"key\":\"email_username\",\"label\":\"邮箱地址\",\"type\":\"input\",\"default\":\"\",\"required\":true},"
             "{\"key\":\"email_password\",\"label\":\"邮箱密码/授权码\",\"type\":\"password\",\"default\":\"\",\"required\":true,\"help\":\"QQ邮箱填授权码\"},"
             "{\"key\":\"email_search_minutes\",\"label\":\"搜索范围(分钟)\",\"type\":\"number\",\"default\":\"30\"}"
             "]')"},

            // ══════════════════════════════════════════════════════
            // v420~v423: 运维面板 (ops panel)
            // ══════════════════════════════════════════════════════
            {420, "create channel_daily_stat",
             "CREATE TABLE IF NOT EXISTS channel_daily_stat("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "stat_date TEXT NOT NULL,"
             "channel_id INTEGER NOT NULL DEFAULT 0,"
             "total_count INTEGER NOT NULL DEFAULT 0,"
             "success_count INTEGER NOT NULL DEFAULT 0,"
             "failed_count INTEGER NOT NULL DEFAULT 0,"
             "pay_amount TEXT NOT NULL DEFAULT '0.00',"
             "fee_amount TEXT NOT NULL DEFAULT '0.00',"
             "refund_count INTEGER NOT NULL DEFAULT 0,"
             "refund_amount TEXT NOT NULL DEFAULT '0.00',"
             "avg_latency_ms REAL NOT NULL DEFAULT 0,"
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "updated_at INTEGER NOT NULL DEFAULT 0,"
             "UNIQUE(stat_date, channel_id))"},
            {421, "index channel_daily_stat",
             "CREATE INDEX IF NOT EXISTS idx_cds_date ON channel_daily_stat(stat_date)"},
            {422, "create ops_deploy_log",
             "CREATE TABLE IF NOT EXISTS ops_deploy_log("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "action TEXT NOT NULL DEFAULT '',"
             "operator TEXT NOT NULL DEFAULT '',"
             "detail TEXT NOT NULL DEFAULT '',"
             "ip TEXT NOT NULL DEFAULT '',"
             "result TEXT NOT NULL DEFAULT '',"
             "created_at INTEGER NOT NULL DEFAULT 0)"},
            {423, "create ops_alert_rule",
             "CREATE TABLE IF NOT EXISTS ops_alert_rule("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "name TEXT NOT NULL DEFAULT '',"
             "rule_type TEXT NOT NULL DEFAULT '',"
             "threshold TEXT NOT NULL DEFAULT '',"
             "level TEXT NOT NULL DEFAULT 'warning',"
             "enabled INTEGER NOT NULL DEFAULT 1,"
             "created_at INTEGER NOT NULL DEFAULT 0)"},

            // ══════════════════════════════════════════════════════
            // v440~v449: WePay V3 原生监控 (新版表结构, 对齐 channel/v3/database.sql)
            // 旧 430~437 已废弃，用新编号避免与旧 schema_version 记录冲突
            // ══════════════════════════════════════════════════════
            {440, "create v3_device",
             "CREATE TABLE IF NOT EXISTS v3_device("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "device_id TEXT NOT NULL UNIQUE,"
             "ip TEXT NOT NULL DEFAULT '',"
             "last_heartbeat INTEGER NOT NULL DEFAULT 0,"
             "online INTEGER NOT NULL DEFAULT 0,"
             "battery INTEGER NOT NULL DEFAULT -1,"
             "network TEXT NOT NULL DEFAULT '',"
             "app_version TEXT NOT NULL DEFAULT '',"
             "screen_resolution TEXT NOT NULL DEFAULT '',"
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "updated_at INTEGER NOT NULL DEFAULT 0);"
             "CREATE INDEX IF NOT EXISTS idx_v3dev_id ON v3_device(device_id);"
             "CREATE INDEX IF NOT EXISTS idx_v3dev_online ON v3_device(online)"},
            {441, "create v3_device_merchant",
             "CREATE TABLE IF NOT EXISTS v3_device_merchant("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "device_id TEXT NOT NULL,"
             "merchant_id TEXT NOT NULL,"
             "bind_time INTEGER NOT NULL DEFAULT 0,"
             "status INTEGER NOT NULL DEFAULT 1,"
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "updated_at INTEGER NOT NULL DEFAULT 0,"
             "UNIQUE(device_id, merchant_id));"
             "CREATE INDEX IF NOT EXISTS idx_v3dm_device ON v3_device_merchant(device_id);"
             "CREATE INDEX IF NOT EXISTS idx_v3dm_merchant ON v3_device_merchant(merchant_id)"},
            {442, "create v3_order",
             "CREATE TABLE IF NOT EXISTS v3_order("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "order_id TEXT NOT NULL UNIQUE,"
             "merchant_order_id TEXT NOT NULL DEFAULT '',"
             "merchant_id TEXT NOT NULL DEFAULT '',"
             "device_id TEXT NOT NULL DEFAULT '',"
             "amount REAL NOT NULL DEFAULT 0,"
             "pay_type TEXT NOT NULL DEFAULT '',"
             "status TEXT NOT NULL DEFAULT 'PENDING',"
             "screenshot_url TEXT NOT NULL DEFAULT '',"
             "idempotent_flag INTEGER NOT NULL DEFAULT 0,"
             "notify_email TEXT NOT NULL DEFAULT '',"
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "pay_time INTEGER NOT NULL DEFAULT 0,"
             "expire_time INTEGER NOT NULL DEFAULT 0,"
             "updated_at INTEGER NOT NULL DEFAULT 0);"
             "CREATE INDEX IF NOT EXISTS idx_v3ord_id ON v3_order(order_id);"
             "CREATE INDEX IF NOT EXISTS idx_v3ord_merchant ON v3_order(merchant_id);"
             "CREATE INDEX IF NOT EXISTS idx_v3ord_device ON v3_order(device_id);"
             "CREATE INDEX IF NOT EXISTS idx_v3ord_status ON v3_order(status)"},
            {443, "create v3_order_status_log + pay_order screenshot_url + plugin_store wepay_v3",
             "CREATE TABLE IF NOT EXISTS v3_order_status_log("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "order_id TEXT NOT NULL DEFAULT '',"
             "old_status TEXT NOT NULL DEFAULT '',"
             "new_status TEXT NOT NULL DEFAULT '',"
             "remark TEXT NOT NULL DEFAULT '',"
             "created_at INTEGER NOT NULL DEFAULT 0);"
             "CREATE INDEX IF NOT EXISTS idx_v3osl_order ON v3_order_status_log(order_id);"
             "ALTER TABLE pay_order ADD COLUMN screenshot_url TEXT NOT NULL DEFAULT '';"
             "INSERT OR IGNORE INTO plugin_store(plugin_code,display_name,description,vendor,"
             "version,icon,plugin_type,supported_ways,price,installed,state,created_at,bg_color,"
             "params_schema) VALUES("
             "'wepay_v3','WePay V3 监控','WePay V3 自研监控插件：sorted-qs HMAC-SHA256签名/"
             "多商户绑定/OCR截图匹配/WebSocket主动推单/MinIO凭证留存/设备状态上报',"
             "'WePay','3.0.0','💎',1,'wxpay,alipay,qqpay',0,0,1,0,'#7c3aed',"
             "'[{\"key\":\"key\",\"label\":\"通讯密钥\",\"type\":\"password\",\"default\":\"\",\"help\":\"HMAC-SHA256共享密钥，留空使用平台密钥\"},"
             "{\"key\":\"require_online\",\"label\":\"要求监控端在线\",\"type\":\"select\",\"default\":\"false\","
             "\"options\":[{\"value\":\"false\",\"label\":\"否\"},{\"value\":\"true\",\"label\":\"是\"}]},"
             "{\"key\":\"replay_window\",\"label\":\"防重放窗口(秒)\",\"type\":\"number\",\"default\":\"60\"},"
             "{\"key\":\"lock_price\",\"label\":\"金额浮动锁\",\"type\":\"select\",\"default\":\"true\","
             "\"options\":[{\"value\":\"true\",\"label\":\"开启\"},{\"value\":\"false\",\"label\":\"关闭\"}]},"
             "{\"key\":\"ocr_enabled\",\"label\":\"允许截图OCR\",\"type\":\"select\",\"default\":\"false\","
             "\"options\":[{\"value\":\"true\",\"label\":\"开启\"},{\"value\":\"false\",\"label\":\"关闭\"}]},"
             "{\"key\":\"ws_push\",\"label\":\"WebSocket推单\",\"type\":\"select\",\"default\":\"true\","
             "\"options\":[{\"value\":\"true\",\"label\":\"开启\"},{\"value\":\"false\",\"label\":\"关闭\"}]}]')"},
            {444, "create v3_merchant_config",
             "CREATE TABLE IF NOT EXISTS v3_merchant_config("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "merchant_id TEXT NOT NULL UNIQUE,"
             "merchant_name TEXT NOT NULL DEFAULT '',"
             "hmac_secret TEXT NOT NULL DEFAULT '',"
             "rsa_public_key TEXT NOT NULL DEFAULT '',"
             "callback_url TEXT NOT NULL DEFAULT '',"
             "notify_email TEXT NOT NULL DEFAULT '',"
             "email_notify_enabled INTEGER NOT NULL DEFAULT 1,"
             "notify_on_success INTEGER NOT NULL DEFAULT 1,"
             "notify_on_fail INTEGER NOT NULL DEFAULT 1,"
             "daily_summary_enabled INTEGER NOT NULL DEFAULT 1,"
             "status INTEGER NOT NULL DEFAULT 1,"
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "updated_at INTEGER NOT NULL DEFAULT 0)"},
            {445, "create v3_system_config",
             "CREATE TABLE IF NOT EXISTS v3_system_config("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "config_key TEXT NOT NULL UNIQUE,"
             "config_value TEXT NOT NULL DEFAULT '',"
             "config_type TEXT NOT NULL DEFAULT 'STRING',"
             "description TEXT NOT NULL DEFAULT '',"
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "updated_at INTEGER NOT NULL DEFAULT 0);"
             "INSERT OR IGNORE INTO v3_system_config(config_key,config_value,config_type,description,created_at) VALUES"
             "('timestamp_window','300','INT','时间戳窗口（秒）',0),"
             "('max_devices','100','INT','最大设备数',0),"
             "('heartbeat_timeout','180','INT','心跳超时（秒）',0),"
             "('order_timeout','300','INT','订单超时（秒）',0),"
             "('enable_hmac','true','BOOL','启用HMAC签名',0),"
             "('enable_rsa','false','BOOL','启用RSA签名',0)"},
            {446, "create v3_security_audit_log",
             "CREATE TABLE IF NOT EXISTS v3_security_audit_log("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "request_ip TEXT NOT NULL DEFAULT '',"
             "device_id TEXT NOT NULL DEFAULT '',"
             "api_path TEXT NOT NULL DEFAULT '',"
             "request_method TEXT NOT NULL DEFAULT '',"
             "sign_result INTEGER NOT NULL DEFAULT 0,"
             "fail_reason TEXT NOT NULL DEFAULT '',"
             "timestamp TEXT NOT NULL DEFAULT '',"
             "nonce TEXT NOT NULL DEFAULT '',"
             "created_at INTEGER NOT NULL DEFAULT 0);"
             "CREATE INDEX IF NOT EXISTS idx_v3sal_ip ON v3_security_audit_log(request_ip);"
             "CREATE INDEX IF NOT EXISTS idx_v3sal_device ON v3_security_audit_log(device_id)"},
            {447, "create v3_email_log",
             "CREATE TABLE IF NOT EXISTS v3_email_log("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "order_id TEXT NOT NULL DEFAULT '',"
             "merchant_id TEXT NOT NULL DEFAULT '',"
             "email_to TEXT NOT NULL DEFAULT '',"
             "email_type TEXT NOT NULL DEFAULT '',"
             "subject TEXT NOT NULL DEFAULT '',"
             "content TEXT NOT NULL DEFAULT '',"
             "send_status INTEGER NOT NULL DEFAULT 0,"
             "send_time INTEGER NOT NULL DEFAULT 0,"
             "fail_reason TEXT NOT NULL DEFAULT '',"
             "retry_count INTEGER NOT NULL DEFAULT 0,"
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "updated_at INTEGER NOT NULL DEFAULT 0);"
             "CREATE INDEX IF NOT EXISTS idx_v3el_order ON v3_email_log(order_id);"
             "CREATE INDEX IF NOT EXISTS idx_v3el_mch ON v3_email_log(merchant_id,created_at)"},
            {448, "create v3_manual_callback_log",
             "CREATE TABLE IF NOT EXISTS v3_manual_callback_log("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "order_id TEXT NOT NULL UNIQUE,"
             "merchant_id TEXT NOT NULL DEFAULT '',"
             "callback_url TEXT NOT NULL DEFAULT '',"
             "callback_token TEXT NOT NULL DEFAULT '',"
             "token_expire INTEGER NOT NULL DEFAULT 0,"
             "callback_status INTEGER NOT NULL DEFAULT 0,"
             "callback_time INTEGER NOT NULL DEFAULT 0,"
             "callback_response TEXT NOT NULL DEFAULT '',"
             "client_ip TEXT NOT NULL DEFAULT '',"
             "user_agent TEXT NOT NULL DEFAULT '',"
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "updated_at INTEGER NOT NULL DEFAULT 0)"},
            {449, "create v3_ip_whitelist",
             "CREATE TABLE IF NOT EXISTS v3_ip_whitelist("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "merchant_id TEXT NOT NULL DEFAULT '',"
             "ip_address TEXT NOT NULL DEFAULT '',"
             "description TEXT NOT NULL DEFAULT '',"
             "status INTEGER NOT NULL DEFAULT 1,"
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "updated_at INTEGER NOT NULL DEFAULT 0);"
             "CREATE INDEX IF NOT EXISTS idx_v3iw_ip ON v3_ip_whitelist(ip_address)"},
            {450, "v3_system_config insert smtp defaults + v3_email_account",
             "INSERT OR IGNORE INTO v3_system_config(config_key,config_value,config_type,description,created_at) VALUES"
             "('smtp_host','','STRING','旧版单账号SMTP（已由v3_email_account替代）',0),"
             "('smtp_port','465','STRING','旧版端口',0),"
             "('smtp_username','','STRING','旧版用户名',0),"
             "('smtp_password','','STRING','旧版授权码',0),"
             "('smtp_from_email','','STRING','旧版发件地址',0),"
             "('smtp_from_name','WePay V3','STRING','旧版发件名',0),"
             "('smtp_use_ssl','true','STRING','旧版SSL开关',0),"
             "('alert_enabled','false','BOOL','告警推送开关',0),"
             "('dingtalk_webhook','','STRING','钉钉Webhook地址',0),"
             "('wecom_webhook','','STRING','企业微信Webhook地址',0),"
             "('alert_email','','STRING','告警接收邮箱',0)"},
            {451, "create v3_email_account",
             "CREATE TABLE IF NOT EXISTS v3_email_account("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "name TEXT NOT NULL DEFAULT '',"
             "smtp_host TEXT NOT NULL DEFAULT '',"
             "smtp_port INTEGER NOT NULL DEFAULT 465,"
             "username TEXT NOT NULL DEFAULT '',"
             "password TEXT NOT NULL DEFAULT '',"
             "from_email TEXT NOT NULL DEFAULT '',"
             "from_name TEXT NOT NULL DEFAULT 'WePay V3',"
             "use_ssl INTEGER NOT NULL DEFAULT 1,"
             "status INTEGER NOT NULL DEFAULT 1,"
             "send_count INTEGER NOT NULL DEFAULT 0,"
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "updated_at INTEGER NOT NULL DEFAULT 0)"},
            {452, "v3_order add notify_email",
             "ALTER TABLE v3_order ADD COLUMN notify_email TEXT NOT NULL DEFAULT ''"},
            {453, "pay_order add buyer_id",
             "ALTER TABLE pay_order ADD COLUMN buyer_id TEXT NOT NULL DEFAULT ''"},
            {454, "pay_order add notify_email",
             "ALTER TABLE pay_order ADD COLUMN notify_email TEXT NOT NULL DEFAULT ''"},
            {455, "create v3_device_qrcode",
             "CREATE TABLE IF NOT EXISTS v3_device_qrcode("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "device_id TEXT NOT NULL,"
             "merchant_id TEXT NOT NULL DEFAULT '',"
             "pay_type TEXT NOT NULL DEFAULT '',"
             "code_type TEXT NOT NULL DEFAULT 'PERSONAL',"
             "code_name TEXT NOT NULL DEFAULT '',"
             "code_content TEXT NOT NULL DEFAULT '',"
             "state INTEGER NOT NULL DEFAULT 1,"
             "sort_order INTEGER NOT NULL DEFAULT 0,"
             "last_used_at INTEGER NOT NULL DEFAULT 0,"
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "updated_at INTEGER NOT NULL DEFAULT 0);"
             "CREATE INDEX IF NOT EXISTS idx_v3qr_device_pay ON v3_device_qrcode(device_id,pay_type,state);"
             "CREATE INDEX IF NOT EXISTS idx_v3qr_merchant ON v3_device_qrcode(merchant_id,state)"},
            {456, "v3_order add qr fields",
             "CREATE TABLE IF NOT EXISTS v3_order("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "order_id TEXT NOT NULL UNIQUE,"
             "merchant_order_id TEXT NOT NULL DEFAULT '',"
             "merchant_id TEXT NOT NULL DEFAULT '',"
             "device_id TEXT NOT NULL DEFAULT '',"
             "amount INTEGER NOT NULL DEFAULT 0,"
             "pay_type TEXT NOT NULL DEFAULT '',"
             "status TEXT NOT NULL DEFAULT 'PENDING',"
             "pay_time INTEGER NOT NULL DEFAULT 0,"
             "expire_time INTEGER NOT NULL DEFAULT 0,"
             "updated_at INTEGER NOT NULL DEFAULT 0,"
             "notify_email TEXT NOT NULL DEFAULT '',"
             "qr_id TEXT NOT NULL DEFAULT '',"
             "qr_code_type TEXT NOT NULL DEFAULT 'PERSONAL',"
             "qr_code_name TEXT NOT NULL DEFAULT '',"
             "qr_code_content TEXT NOT NULL DEFAULT '');"
             "CREATE INDEX IF NOT EXISTS idx_v3ord_id ON v3_order(order_id);"
             "CREATE INDEX IF NOT EXISTS idx_v3ord_merchant ON v3_order(merchant_id);"
             "CREATE INDEX IF NOT EXISTS idx_v3ord_device ON v3_order(device_id);"
             "CREATE INDEX IF NOT EXISTS idx_v3ord_status ON v3_order(status)"},
            {457, "pay_channel add biz settings",
             "ALTER TABLE pay_channel ADD COLUMN select_mode INTEGER NOT NULL DEFAULT 0;"
             "ALTER TABLE pay_channel ADD COLUMN code_type TEXT NOT NULL DEFAULT '';"
             "ALTER TABLE pay_channel ADD COLUMN support_business_code INTEGER NOT NULL DEFAULT 0"},
            {458, "merchant_channel add biz settings",
             "ALTER TABLE merchant_channel ADD COLUMN select_mode INTEGER NOT NULL DEFAULT 0;"
             "ALTER TABLE merchant_channel ADD COLUMN code_type TEXT NOT NULL DEFAULT ''"},

            // ══════════════════════════════════════════════════════
            // v460~v465: 设备密钥登录（EdDSA/RSA）
            // ══════════════════════════════════════════════════════
            {460, "create device_keys",
             "CREATE TABLE IF NOT EXISTS device_keys("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "user_type INTEGER NOT NULL DEFAULT 0,"        // 1=管理员 2=商户
             "user_id INTEGER NOT NULL DEFAULT 0,"          // 关联 sys_user.id 或 merchant.id
             "username TEXT NOT NULL DEFAULT '',"           // 用户名（冗余，方便查询）
             "key_type TEXT NOT NULL DEFAULT 'ed25519',"    // ed25519 / rsa2048 / rsa4096
             "public_key TEXT NOT NULL,"                    // 公钥（Ed25519=hex, RSA=PEM）
             "device_name TEXT NOT NULL DEFAULT '',"        // 设备名称（如"我的iPhone"）
             "device_info TEXT NOT NULL DEFAULT '',"        // 设备信息JSON（UA、OS、IP等）
             "last_used_at INTEGER NOT NULL DEFAULT 0,"     // 最后使用时间
             "last_used_ip TEXT NOT NULL DEFAULT '',"       // 最后使用IP
             "require_2fa INTEGER NOT NULL DEFAULT 0,"      // 是否要求二次验证（0=否 1=是）
             "trusted_ips TEXT NOT NULL DEFAULT '',"        // 可信IP列表（JSON数组，空=不限制）
             "require_ip_verify INTEGER NOT NULL DEFAULT 1," // 是否要求IP验证（1=是 0=否，本地IP除外）
             "state INTEGER NOT NULL DEFAULT 1,"            // 1=启用 0=禁用
             "created_at INTEGER NOT NULL DEFAULT 0,"
             "updated_at INTEGER NOT NULL DEFAULT 0)"},
            {461, "index device_keys user",
             "CREATE INDEX IF NOT EXISTS idx_device_keys_user ON device_keys(user_type, user_id, state)"},
            {462, "index device_keys public_key",
             "CREATE INDEX IF NOT EXISTS idx_device_keys_pk ON device_keys(public_key)"},
            {463, "unique device_keys user+key",
             "CREATE UNIQUE INDEX IF NOT EXISTS idx_device_keys_unique ON device_keys(user_type, user_id, public_key)"},
            {464, "pay_order add qr_path",
             "ALTER TABLE pay_order ADD COLUMN qr_path TEXT DEFAULT ''"},
            {465, "print_task add retry_cnt",
             "ALTER TABLE print_task ADD COLUMN retry_cnt INTEGER NOT NULL DEFAULT 0"},
            {466, "idx print_task updated_at",
             "CREATE INDEX IF NOT EXISTS idx_ptask_updated ON print_task(updated_at)"},
            {324, "v3_order backfill pay_notify_task (SQLite only)",
             "INSERT OR IGNORE INTO pay_notify_task"
             "(order_id,plugin,notify_full_url,status,retry_cnt,next_retry_at,created_at,updated_at) "
             "SELECT vo.order_id, 'WepayV3Plugin', "
             "COALESCE(po.notify_url, vmc.callback_url, ''), "
             "1, 0, 0, vo.created_at, vo.updated_at "
             "FROM v3_order vo "
             "LEFT JOIN pay_order po ON po.order_id = vo.order_id "
             "LEFT JOIN v3_merchant_config vmc ON vmc.merchant_id = vo.merchant_id "
             "WHERE vo.status = 'PAID' "
             "AND vo.order_id NOT IN (SELECT order_id FROM pay_notify_task) "
             "AND (po.notify_url != '' OR vmc.callback_url != '') "
             "UNION ALL SELECT '' WHERE 0"}
        };

        for (auto &m : MIGS) {
            auto exist = db.isUsingSqlite()
                ? db.querySqliteDirect(
                      "SELECT 1 FROM schema_version WHERE version_id=?",
                      {std::to_string(m.ver)})
                : db.query(
                      "SELECT 1 FROM schema_version WHERE version_id=?",
                      {std::to_string(m.ver)});
            if (!exist.empty()) continue;

            // v324: SQLite-only migration，PG 上跳过
            if (m.ver == 324 && !db.isUsingSqlite()) continue;

            // v150~v157: 旧列名兼容迁移，失败说明表已是新列名，静默跳过
            bool isCompatRename = (m.ver >= 150 && m.ver <= 157);

            std::string sql = m.sql;
            bool ok;
            if (db.isUsingSqlite()) {
                ok = db.execSqliteDirect(sql);
            } else {
                // PG 主库执行(PG方言)，不自动同步从库
                ok = db.execPgOnly(sqlToPg(sql));
                // SQLite 从库用原始 SQLite 方言执行（避免 PG→SQLite 双重转换丢失）
                db.execSqliteDirect(sql);
            }

            // 只有成功才标记版本号，失败则下次启动时重试
            std::string verStr = std::to_string(m.ver);
            if (ok || isCompatRename) {
                if (db.isUsingSqlite()) {
                    db.execSqliteDirect(
                        "INSERT OR IGNORE INTO schema_version(version_id,description) VALUES(?,?)",
                        {verStr, m.desc});
                } else {
                    db.execPgOnly(
                        "INSERT INTO schema_version(version_id,description) VALUES(?,?) "
                        "ON CONFLICT(version_id) DO NOTHING",
                        {verStr, m.desc});
                    db.execSqliteDirect(
                        "INSERT OR IGNORE INTO schema_version(version_id,description) VALUES(?,?)",
                        {verStr, m.desc});
                }
            }

            if (ok) {
                LOG_INFO << "[Migration] v" << m.ver << " applied: " << m.desc;
            } else if (isCompatRename) {
                // 旧列不存在，说明数据库已是新 schema，静默忽略
            } else {
                LOG_ERROR << "[Migration] v" << m.ver << " FAILED: " << m.desc;
            }
        }
    }

    // ── 默认数据 ────────────────────────────────────────────────
    static void insertDefaults(PayDb &db) {
        struct KV { const char *k, *v; };
        static const KV defaults[] = {
            {"siteName",       "WePay"},
            {"admin_user",     "admin"},
            {"admin_pass",     "admin"},  // 首次登录后应修改
            {"close_minutes",  "5"},
            {"payQf",          "1"},
            {"order_prefix",   "W"},
        };
        for (auto &kv : defaults) {
            auto row = db.queryOne("SELECT vvalue FROM setting WHERE vkey=?", {kv.k});
            if (row.empty()) {
                if (db.isUsingSqlite())
                    db.execSqliteDirect("INSERT INTO setting(vkey,vvalue) VALUES(?,?)", {kv.k, kv.v});
                else
                    db.exec("INSERT INTO setting(vkey,vvalue) VALUES(?,?) "
                            "ON CONFLICT(vkey) DO NOTHING", {kv.k, kv.v});
            }
        }

        // 默认支付类型
        struct PT { const char *code; const char *name; };
        static const PT payTypes[] = {
            {"wxpay",    "微信支付"},
            {"alipay",   "支付宝"},
            {"qqpay",    "QQ钱包"},
            {"bankpay",  "银行卡"},
            {"usdt",     "USDT"},
        };
        for (auto &pt : payTypes) {
            auto row = db.queryOne("SELECT id FROM pay_type WHERE code=?", {pt.code});
            if (row.empty()) {
                if (db.isUsingSqlite())
                    db.execSqliteDirect("INSERT INTO pay_type(code,name,state) VALUES(?,?,1)",
                                        {pt.code, pt.name});
                else
                    db.exec("INSERT INTO pay_type(code,name,state) VALUES(?,?,1) "
                            "ON CONFLICT(code) DO NOTHING", {pt.code, pt.name});
            }
        }

        insertRbacDefaults(db);
    }

    // ── RBAC 默认数据 ──────────────────────────────────────────
    static void insertRbacDefaults(PayDb &db) {
        // 默认权限清单
        struct Perm { const char *code, *name, *module; };
        static const Perm perms[] = {
            {"dashboard:view",   "数据面板查看", "dashboard"},
            {"merchant:view",    "商户查看",    "merchant"},
            {"merchant:add",     "商户新增",    "merchant"},
            {"merchant:edit",    "商户编辑",    "merchant"},
            {"merchant:delete",  "商户删除",    "merchant"},
            {"agent:view",       "代理查看",    "agent"},
            {"agent:add",        "代理新增",    "agent"},
            {"agent:edit",       "代理编辑",    "agent"},
            {"agent:delete",     "代理删除",    "agent"},
            {"channel:view",     "通道查看",    "channel"},
            {"channel:edit",     "通道编辑",    "channel"},
            {"order:view",       "订单查看",    "order"},
            {"order:close",      "订单关闭",    "order"},
            {"order:reissue",    "订单补单",    "order"},
            {"order:refund",     "订单退款",    "order"},
            {"settle:view",      "结算查看",    "settle"},
            {"settle:approve",   "结算审核",    "settle"},
            {"transfer:view",    "转账查看",    "transfer"},
            {"transfer:create",  "发起转账",    "transfer"},
            {"division:view",    "分账查看",    "division"},
            {"division:manage",  "分账管理",    "division"},
            {"config:view",      "系统配置",    "config"},
            {"config:edit",      "配置修改",    "config"},
            {"sysuser:manage",   "用户管理",    "sysuser"},
            {"role:manage",      "角色管理",    "role"},
            {"oplog:view",       "操作日志",    "oplog"},
        };
        for (auto &p : perms) {
            auto row = db.queryOne("SELECT id FROM sys_permission WHERE perm_code=?", {p.code});
            if (!row.empty()) continue;
            auto sql = "INSERT INTO sys_permission(perm_code,perm_name,module,created_at) VALUES(?,?,?,?)";
            std::string now = std::to_string(std::time(nullptr));
            if (db.isUsingSqlite())
                db.execSqliteDirect(sql, {p.code, p.name, p.module, now});
            else
                db.exec(std::string(sql) + " ON CONFLICT(perm_code) DO NOTHING",
                        {p.code, p.name, p.module, now});
        }

        // 默认角色: super_admin / operator / finance / readonly
        struct Role { const char *code, *name, *remark; };
        static const Role roles[] = {
            {"super_admin", "超级管理员", "拥有所有权限"},
            {"operator",    "运营",       "日常运营(商户/订单/通道)"},
            {"finance",     "财务",       "结算审核/资金查看"},
            {"readonly",    "只读",       "仅可查看"},
        };
        for (auto &r : roles) {
            auto row = db.queryOne("SELECT id FROM sys_role WHERE role_code=?", {r.code});
            if (!row.empty()) continue;
            std::string now = std::to_string(std::time(nullptr));
            if (db.isUsingSqlite())
                db.execSqliteDirect(
                    "INSERT INTO sys_role(role_code,role_name,remark,state,created_at) VALUES(?,?,?,1,?)",
                    {r.code, r.name, r.remark, now});
            else
                db.exec(
                    "INSERT INTO sys_role(role_code,role_name,remark,state,created_at) VALUES(?,?,?,1,?) "
                    "ON CONFLICT(role_code) DO NOTHING",
                    {r.code, r.name, r.remark, now});
        }

        // 默认超级管理员账号(与原 setting.admin_user/pass 兼容)
        auto sysUser = db.queryOne("SELECT id FROM sys_user WHERE username=?", {"admin"});
        if (sysUser.empty()) {
            std::string now = std::to_string(std::time(nullptr));
            // 此处用明文占位(首次登录逻辑会自动升级为加盐哈希)
            if (db.isUsingSqlite())
                db.execSqliteDirect(
                    "INSERT INTO sys_user(username,password,salt,real_name,is_super,state,created_at,updated_at) "
                    "VALUES('admin','admin','','系统管理员',1,1,?,?)", {now, now});
            else
                db.exec(
                    "INSERT INTO sys_user(username,password,salt,real_name,is_super,state,created_at,updated_at) "
                    "VALUES('admin','admin','','系统管理员',1,1,?,?) ON CONFLICT(username) DO NOTHING",
                    {now, now});

            // 绑定超级管理员角色
            db.exec("INSERT INTO sys_user_role(user_id,role_id) "
                    "SELECT u.id,r.id FROM sys_user u, sys_role r "
                    "WHERE u.username='admin' AND r.role_code='super_admin'");
        }
    }

    // ══════════════════════════════════════════════════════════════
    //  SQLite DDL — 完整多商户支付系统表结构
    // ══════════════════════════════════════════════════════════════
    static std::vector<std::string> getSqliteCreateSqls() {
        return {
            // ── 通用 KV 设置 ──────────────────────────────────
            "CREATE TABLE IF NOT EXISTS setting("
            "vkey TEXT PRIMARY KEY,"
            "vvalue TEXT NOT NULL DEFAULT '')",

            // ── 支付类型 (wxpay/alipay/qqpay/...) ─────────────
            "CREATE TABLE IF NOT EXISTS pay_type("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "code TEXT NOT NULL UNIQUE,"
            "name TEXT NOT NULL DEFAULT '',"
            "icon TEXT NOT NULL DEFAULT '',"
            "state INTEGER NOT NULL DEFAULT 1,"
            "sort_order INTEGER NOT NULL DEFAULT 0)",

            // ── 商户 ─────────────────────────────────────────
            "CREATE TABLE IF NOT EXISTS merchant("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "mch_no TEXT NOT NULL UNIQUE,"           // 商户号 M100001
            "username TEXT NOT NULL UNIQUE,"          // 登录名
            "password TEXT NOT NULL,"                 // sha256(salt+pwd)
            "salt TEXT NOT NULL DEFAULT '',"
            "mch_name TEXT NOT NULL DEFAULT '',"      // 商户名称
            "contact TEXT NOT NULL DEFAULT '',"       // 联系人
            "phone TEXT NOT NULL DEFAULT '',"
            "email TEXT NOT NULL DEFAULT '',"
            "mch_key TEXT NOT NULL DEFAULT '',"       // 通讯密钥
            "sign_type TEXT NOT NULL DEFAULT 'MD5',"  // MD5 / RSA
            "public_key TEXT NOT NULL DEFAULT '',"    // RSA公钥
            "balance TEXT NOT NULL DEFAULT '0.00',"   // 可用余额
            "frozen TEXT NOT NULL DEFAULT '0.00',"    // 冻结金额
            "total_income TEXT NOT NULL DEFAULT '0.00',"
            "rate TEXT NOT NULL DEFAULT '1.00',"      // 默认费率(百分比)
            "settle_type INTEGER NOT NULL DEFAULT 0," // 0=手动 1=自动 T+1
            "settle_account TEXT NOT NULL DEFAULT '',"// 结算账户信息JSON
            "ip_white TEXT NOT NULL DEFAULT '',"      // IP白名单,逗号分隔
            "notify_url TEXT NOT NULL DEFAULT '',"    // 默认通知地址
            "return_url TEXT NOT NULL DEFAULT '',"    // 默认跳转地址
            "state INTEGER NOT NULL DEFAULT 1,"       // 0=禁用 1=正常
            "created_at INTEGER NOT NULL DEFAULT 0,"
            "updated_at INTEGER NOT NULL DEFAULT 0)",

            // ── 商户应用 ──────────────────────────────────────
            "CREATE TABLE IF NOT EXISTS merchant_app("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "mch_id INTEGER NOT NULL,"               // 关联 merchant.id
            "app_id TEXT NOT NULL UNIQUE,"            // 应用ID
            "app_name TEXT NOT NULL DEFAULT '',"
            "app_key TEXT NOT NULL DEFAULT '',"       // 应用密钥
            "notify_url TEXT NOT NULL DEFAULT '',"
            "return_url TEXT NOT NULL DEFAULT '',"
            "state INTEGER NOT NULL DEFAULT 1,"
            "created_at INTEGER NOT NULL DEFAULT 0)",

            // ── 支付通道 ─────────────────────────────────────
            "CREATE TABLE IF NOT EXISTS pay_channel("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "channel_code TEXT NOT NULL UNIQUE,"      // alipay_native / wxpay_h5 / vmq_wxpay...
            "channel_name TEXT NOT NULL DEFAULT '',"
            "pay_type TEXT NOT NULL DEFAULT '',"       // 关联 pay_type.code
            "plugin TEXT NOT NULL DEFAULT '',"         // 插件标识: vmq / epay / official_wx / ...
            "rate TEXT NOT NULL DEFAULT '1.00',"       // 通道费率(百分比)
            "params_json TEXT NOT NULL DEFAULT '{}',"  // 通道参数JSON(API密钥等)
            "min_amount TEXT NOT NULL DEFAULT '0.01',"
            "max_amount TEXT NOT NULL DEFAULT '50000',"
            "day_limit TEXT NOT NULL DEFAULT '0',"     // 0=不限
            "state INTEGER NOT NULL DEFAULT 1,"        // 0=关闭 1=开启
            "sort_order INTEGER NOT NULL DEFAULT 0,"
            "remark TEXT NOT NULL DEFAULT '',"
            "created_at INTEGER NOT NULL DEFAULT 0,"
            "updated_at INTEGER NOT NULL DEFAULT 0)",

            // ── 通道账户(一个通道下可有多套参数轮询) ──────────
            "CREATE TABLE IF NOT EXISTS pay_channel_account("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "channel_id INTEGER NOT NULL,"
            "account_name TEXT NOT NULL DEFAULT '',"
            "params_json TEXT NOT NULL DEFAULT '{}',"  // 此账户的参数(商户号、密钥等)
            "weight INTEGER NOT NULL DEFAULT 1,"       // 轮询权重
            "state INTEGER NOT NULL DEFAULT 1,"
            "created_at INTEGER NOT NULL DEFAULT 0)",

            // ── 商户-通道绑定 ─────────────────────────────────
            "CREATE TABLE IF NOT EXISTS merchant_channel("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "mch_id INTEGER NOT NULL,"
            "channel_id INTEGER NOT NULL,"
            "rate TEXT NOT NULL DEFAULT '',"            // 商户自定义费率(空=用通道默认)
            "state INTEGER NOT NULL DEFAULT 1,"
            "UNIQUE(mch_id, channel_id))",

            // ── 支付订单 ─────────────────────────────────────
            "CREATE TABLE IF NOT EXISTS pay_order("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "order_id TEXT NOT NULL UNIQUE,"            // 系统订单号
            "mch_id INTEGER NOT NULL DEFAULT 0,"        // 商户ID
            "app_id TEXT NOT NULL DEFAULT '',"           // 应用ID
            "mch_order_no TEXT NOT NULL DEFAULT '',"     // 商户订单号(原pay_id)
            "channel_id INTEGER NOT NULL DEFAULT 0,"     // 通道ID
            "pay_type TEXT NOT NULL DEFAULT '',"          // wxpay / alipay / ...
            "amount TEXT NOT NULL DEFAULT '0.00',"        // 订单金额
            "real_amount TEXT NOT NULL DEFAULT '0.00',"   // 实付金额
            "mch_fee_rate TEXT NOT NULL DEFAULT '0',"     // 商户费率
            "mch_fee_amount TEXT NOT NULL DEFAULT '0',"   // 商户手续费
            "channel_fee_rate TEXT NOT NULL DEFAULT '0'," // 通道费率
            "channel_fee_amount TEXT NOT NULL DEFAULT '0',"// 通道成本
            "subject TEXT NOT NULL DEFAULT '',"           // 商品名称
            "body TEXT NOT NULL DEFAULT '',"              // 商品描述
            "param TEXT DEFAULT '',"                      // 透传参数
            "pay_url TEXT DEFAULT '',"                    // 支付链接/二维码
            "qr_path TEXT DEFAULT '',"                    // 二维码图片路径
            "notify_url TEXT DEFAULT '',"
            "return_url TEXT DEFAULT '',"
            "buyer_id TEXT NOT NULL DEFAULT '',"          // 买家标识
            "client_ip TEXT NOT NULL DEFAULT '',"
            "device TEXT NOT NULL DEFAULT '',"            // 设备标识
            "channel_order_no TEXT NOT NULL DEFAULT '',"  // 上游/通道订单号
            "channel_data TEXT NOT NULL DEFAULT '',"      // 通道返回的原始数据
            "state INTEGER NOT NULL DEFAULT 0,"           // -1=已关闭 0=待支付 1=已支付 2=已退款
            "notify_state INTEGER NOT NULL DEFAULT 0,"    // 0=未通知 1=已通知 2=通知失败
            "refund_amount TEXT NOT NULL DEFAULT '0.00',"
            "expire_time INTEGER NOT NULL DEFAULT 0,"
            "pay_time INTEGER NOT NULL DEFAULT 0,"
            "created_at INTEGER NOT NULL DEFAULT 0,"
            "updated_at INTEGER NOT NULL DEFAULT 0)",

            // ── 退款订单 ─────────────────────────────────────
            "CREATE TABLE IF NOT EXISTS refund_order("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "refund_no TEXT NOT NULL UNIQUE,"
            "order_id TEXT NOT NULL,"                    // 关联 pay_order.order_id
            "mch_id INTEGER NOT NULL,"
            "mch_refund_no TEXT NOT NULL DEFAULT '',"     // 商户退款单号
            "channel_id INTEGER NOT NULL DEFAULT 0,"
            "pay_type TEXT NOT NULL DEFAULT '',"
            "pay_amount TEXT NOT NULL DEFAULT '0.00',"    // 原订单金额
            "refund_amount TEXT NOT NULL DEFAULT '0.00',"
            "refund_fee TEXT NOT NULL DEFAULT '0.00',"    // 退还手续费
            "reason TEXT NOT NULL DEFAULT '',"
            "channel_refund_no TEXT NOT NULL DEFAULT '',"
            "state INTEGER NOT NULL DEFAULT 0,"           // 0=待处理 1=成功 2=失败
            "created_at INTEGER NOT NULL DEFAULT 0,"
            "updated_at INTEGER NOT NULL DEFAULT 0)",

            // ── 结算订单 ─────────────────────────────────────
            "CREATE TABLE IF NOT EXISTS settle_order("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "settle_no TEXT NOT NULL UNIQUE,"
            "mch_id INTEGER NOT NULL,"
            "amount TEXT NOT NULL DEFAULT '0.00',"        // 结算金额
            "fee TEXT NOT NULL DEFAULT '0.00',"            // 结算手续费
            "real_amount TEXT NOT NULL DEFAULT '0.00',"    // 实际到账
            "account_info TEXT NOT NULL DEFAULT '',"       // 结算账户JSON
            "state INTEGER NOT NULL DEFAULT 0,"            // 0=待审 1=处理中 2=成功 3=驳回
            "admin_remark TEXT NOT NULL DEFAULT '',"
            "created_at INTEGER NOT NULL DEFAULT 0,"
            "updated_at INTEGER NOT NULL DEFAULT 0)",

            // ── 资金日志 ─────────────────────────────────────
            "CREATE TABLE IF NOT EXISTS money_log("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "mch_id INTEGER NOT NULL,"
            "change_type INTEGER NOT NULL,"              // 1=收入 2=支出 3=冻结 4=解冻 5=结算 6=退款
            "change_amount TEXT NOT NULL DEFAULT '0.00',"
            "before_amount TEXT NOT NULL DEFAULT '0.00',"
            "after_amount TEXT NOT NULL DEFAULT '0.00',"
            "biz_type TEXT NOT NULL DEFAULT '',"          // order/settle/refund/manual
            "biz_no TEXT NOT NULL DEFAULT '',"            // 关联单号
            "remark TEXT NOT NULL DEFAULT '',"
            "created_at INTEGER NOT NULL DEFAULT 0)",

            // ── 设备 (OCR/安卓监听/挂机) ─────────────────────
            "CREATE TABLE IF NOT EXISTS device("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "device_no TEXT NOT NULL UNIQUE,"             // 设备编号
            "device_name TEXT NOT NULL DEFAULT '',"
            "device_type INTEGER NOT NULL DEFAULT 0,"     // 0=未知 1=安卓监听 2=PC挂机 3=OCR 4=云端
            "mch_id INTEGER NOT NULL DEFAULT 0,"          // 归属商户(0=平台)
            "channel_id INTEGER NOT NULL DEFAULT 0,"      // 关联通道
            "bind_account TEXT NOT NULL DEFAULT '',"      // 绑定收款账号
            "pay_types TEXT NOT NULL DEFAULT '',"          // 支持的支付类型,逗号分隔
            "last_heart INTEGER NOT NULL DEFAULT 0,"
            "last_pay INTEGER NOT NULL DEFAULT 0,"
            "state INTEGER NOT NULL DEFAULT 0,"           // 0=离线 1=在线 2=异常
            "extra_json TEXT NOT NULL DEFAULT '{}',"       // 额外参数
            "created_at INTEGER NOT NULL DEFAULT 0,"
            "updated_at INTEGER NOT NULL DEFAULT 0)",

            // ── 收款码(兼容旧版) ─────────────────────────────
            "CREATE TABLE IF NOT EXISTS pay_qrcode("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "mch_id INTEGER NOT NULL DEFAULT 0,"
            "device_id INTEGER NOT NULL DEFAULT 0,"
            "type INTEGER NOT NULL,"
            "pay_url TEXT NOT NULL,"
            "price TEXT DEFAULT '0',"
            "state INTEGER NOT NULL DEFAULT 0,"
            "account TEXT NOT NULL DEFAULT '',"
            "pattern INTEGER NOT NULL DEFAULT 1,"
            "created_at INTEGER NOT NULL DEFAULT 0)",

            // ── 临时金额锁 ───────────────────────────────────
            "CREATE TABLE IF NOT EXISTS tmp_price("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "price TEXT NOT NULL,"
            "oid TEXT NOT NULL,"
            "UNIQUE(price))",

            // ── 通知任务队列 ─────────────────────────────────
            "CREATE TABLE IF NOT EXISTS pay_notify_task("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "order_id TEXT NOT NULL UNIQUE,"
            "plugin TEXT NOT NULL DEFAULT '',"
            "notify_full_url TEXT NOT NULL,"
            "status INTEGER NOT NULL DEFAULT 0,"
            "retry_cnt INTEGER NOT NULL DEFAULT 0,"
            "next_retry_at INTEGER NOT NULL DEFAULT 0,"
            "last_response TEXT DEFAULT '',"
            "created_at INTEGER NOT NULL,"
            "updated_at INTEGER NOT NULL)",

            // ── 回调日志 ─────────────────────────────────────
            "CREATE TABLE IF NOT EXISTS pay_callback_log("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "order_id TEXT NOT NULL,"
            "notify_url TEXT DEFAULT '',"
            "http_status INTEGER NOT NULL DEFAULT 0,"
            "response TEXT DEFAULT '',"
            "success INTEGER NOT NULL DEFAULT 0,"
            "created_at INTEGER NOT NULL)",

            // ── 操作日志 ─────────────────────────────────────
            "CREATE TABLE IF NOT EXISTS oplog("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "user_type INTEGER NOT NULL DEFAULT 0,"      // 0=admin 1=merchant
            "user_id INTEGER NOT NULL DEFAULT 0,"
            "username TEXT NOT NULL DEFAULT '',"
            "action TEXT NOT NULL DEFAULT '',"
            "target TEXT NOT NULL DEFAULT '',"
            "detail TEXT NOT NULL DEFAULT '',"
            "ip TEXT NOT NULL DEFAULT '',"
            "created_at INTEGER NOT NULL DEFAULT 0)",

            // ── 商户 Webhook 配置 ─────────────────────────────
            "CREATE TABLE IF NOT EXISTS mch_webhook("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "mch_id INTEGER NOT NULL,"
            "event_type TEXT NOT NULL DEFAULT '',"        // payment/refund/settle
            "notify_url TEXT NOT NULL DEFAULT '',"
            "secret_key TEXT NOT NULL DEFAULT '',"         // HMAC 密钥，用于签名
            "sign_type TEXT NOT NULL DEFAULT 'HMAC-SHA256'," // HMAC-SHA256 / RSA-SHA256
            "state INTEGER NOT NULL DEFAULT 1,"           // 0=禁用 1=启用
            "retry_count INTEGER NOT NULL DEFAULT 0,"
            "retry_max INTEGER NOT NULL DEFAULT 3,"
            "timeout_sec INTEGER NOT NULL DEFAULT 30,"
            "created_at INTEGER NOT NULL DEFAULT 0,"
            "updated_at INTEGER NOT NULL DEFAULT 0)",
            "CREATE INDEX IF NOT EXISTS idx_mch_wh_mch ON mch_webhook(mch_id, event_type, state)",
        };
    }

    // ══════════════════════════════════════════════════════════════
    //  PG DDL
    // ══════════════════════════════════════════════════════════════
    static std::vector<std::string> getPgCreateSqls() {
        return {
            "CREATE TABLE IF NOT EXISTS setting("
            "vkey TEXT PRIMARY KEY,"
            "vvalue TEXT NOT NULL DEFAULT '')",

            "CREATE TABLE IF NOT EXISTS pay_type("
            "id BIGSERIAL PRIMARY KEY,"
            "code TEXT NOT NULL UNIQUE,"
            "name TEXT NOT NULL DEFAULT '',"
            "icon TEXT NOT NULL DEFAULT '',"
            "state INTEGER NOT NULL DEFAULT 1,"
            "sort_order INTEGER NOT NULL DEFAULT 0)",

            "CREATE TABLE IF NOT EXISTS merchant("
            "id BIGSERIAL PRIMARY KEY,"
            "mch_no TEXT NOT NULL UNIQUE,"
            "username TEXT NOT NULL UNIQUE,"
            "password TEXT NOT NULL,"
            "salt TEXT NOT NULL DEFAULT '',"
            "mch_name TEXT NOT NULL DEFAULT '',"
            "contact TEXT NOT NULL DEFAULT '',"
            "phone TEXT NOT NULL DEFAULT '',"
            "email TEXT NOT NULL DEFAULT '',"
            "mch_key TEXT NOT NULL DEFAULT '',"
            "sign_type TEXT NOT NULL DEFAULT 'MD5',"
            "public_key TEXT NOT NULL DEFAULT '',"
            "balance TEXT NOT NULL DEFAULT '0.00',"
            "frozen TEXT NOT NULL DEFAULT '0.00',"
            "total_income TEXT NOT NULL DEFAULT '0.00',"
            "rate TEXT NOT NULL DEFAULT '1.00',"
            "settle_type INTEGER NOT NULL DEFAULT 0,"
            "settle_account TEXT NOT NULL DEFAULT '',"
            "ip_white TEXT NOT NULL DEFAULT '',"
            "notify_url TEXT NOT NULL DEFAULT '',"
            "return_url TEXT NOT NULL DEFAULT '',"
            "state INTEGER NOT NULL DEFAULT 1,"
            "created_at BIGINT NOT NULL DEFAULT 0,"
            "updated_at BIGINT NOT NULL DEFAULT 0)",

            "CREATE TABLE IF NOT EXISTS merchant_app("
            "id BIGSERIAL PRIMARY KEY,"
            "mch_id BIGINT NOT NULL,"
            "app_id TEXT NOT NULL UNIQUE,"
            "app_name TEXT NOT NULL DEFAULT '',"
            "app_key TEXT NOT NULL DEFAULT '',"
            "notify_url TEXT NOT NULL DEFAULT '',"
            "return_url TEXT NOT NULL DEFAULT '',"
            "state INTEGER NOT NULL DEFAULT 1,"
            "created_at BIGINT NOT NULL DEFAULT 0)",

            "CREATE TABLE IF NOT EXISTS pay_channel("
            "id BIGSERIAL PRIMARY KEY,"
            "channel_code TEXT NOT NULL UNIQUE,"
            "channel_name TEXT NOT NULL DEFAULT '',"
            "pay_type TEXT NOT NULL DEFAULT '',"
            "plugin TEXT NOT NULL DEFAULT '',"
            "rate TEXT NOT NULL DEFAULT '1.00',"
            "params_json TEXT NOT NULL DEFAULT '{}',"
            "min_amount TEXT NOT NULL DEFAULT '0.01',"
            "max_amount TEXT NOT NULL DEFAULT '50000',"
            "day_limit TEXT NOT NULL DEFAULT '0',"
            "state INTEGER NOT NULL DEFAULT 1,"
            "sort_order INTEGER NOT NULL DEFAULT 0,"
            "remark TEXT NOT NULL DEFAULT '',"
            "created_at BIGINT NOT NULL DEFAULT 0,"
            "updated_at BIGINT NOT NULL DEFAULT 0)",

            "CREATE TABLE IF NOT EXISTS pay_channel_account("
            "id BIGSERIAL PRIMARY KEY,"
            "channel_id BIGINT NOT NULL,"
            "account_name TEXT NOT NULL DEFAULT '',"
            "params_json TEXT NOT NULL DEFAULT '{}',"
            "weight INTEGER NOT NULL DEFAULT 1,"
            "state INTEGER NOT NULL DEFAULT 1,"
            "created_at BIGINT NOT NULL DEFAULT 0)",

            "CREATE TABLE IF NOT EXISTS merchant_channel("
            "id BIGSERIAL PRIMARY KEY,"
            "mch_id BIGINT NOT NULL,"
            "channel_id BIGINT NOT NULL,"
            "rate TEXT NOT NULL DEFAULT '',"
            "state INTEGER NOT NULL DEFAULT 1,"
            "UNIQUE(mch_id, channel_id))",

            "CREATE TABLE IF NOT EXISTS pay_order("
            "id BIGSERIAL PRIMARY KEY,"
            "order_id TEXT NOT NULL UNIQUE,"
            "mch_id BIGINT NOT NULL DEFAULT 0,"
            "app_id TEXT NOT NULL DEFAULT '',"
            "mch_order_no TEXT NOT NULL DEFAULT '',"
            "channel_id BIGINT NOT NULL DEFAULT 0,"
            "pay_type TEXT NOT NULL DEFAULT '',"
            "amount TEXT NOT NULL DEFAULT '0.00',"
            "real_amount TEXT NOT NULL DEFAULT '0.00',"
            "mch_fee_rate TEXT NOT NULL DEFAULT '0',"
            "mch_fee_amount TEXT NOT NULL DEFAULT '0',"
            "channel_fee_rate TEXT NOT NULL DEFAULT '0',"
            "channel_fee_amount TEXT NOT NULL DEFAULT '0',"
            "subject TEXT NOT NULL DEFAULT '',"
            "body TEXT NOT NULL DEFAULT '',"
            "param TEXT DEFAULT '',"
            "pay_url TEXT DEFAULT '',"
            "qr_path TEXT DEFAULT '',"
            "notify_url TEXT DEFAULT '',"
            "return_url TEXT DEFAULT '',"
            "buyer_id TEXT NOT NULL DEFAULT '',"
            "client_ip TEXT NOT NULL DEFAULT '',"
            "device TEXT NOT NULL DEFAULT '',"
            "channel_order_no TEXT NOT NULL DEFAULT '',"
            "channel_data TEXT NOT NULL DEFAULT '',"
            "state INTEGER NOT NULL DEFAULT 0,"
            "notify_state INTEGER NOT NULL DEFAULT 0,"
            "refund_amount TEXT NOT NULL DEFAULT '0.00',"
            "expire_time BIGINT NOT NULL DEFAULT 0,"
            "pay_time BIGINT NOT NULL DEFAULT 0,"
            "created_at BIGINT NOT NULL DEFAULT 0,"
            "updated_at BIGINT NOT NULL DEFAULT 0)",

            "CREATE TABLE IF NOT EXISTS refund_order("
            "id BIGSERIAL PRIMARY KEY,"
            "refund_no TEXT NOT NULL UNIQUE,"
            "order_id TEXT NOT NULL,"
            "mch_id BIGINT NOT NULL,"
            "mch_refund_no TEXT NOT NULL DEFAULT '',"
            "channel_id BIGINT NOT NULL DEFAULT 0,"
            "pay_type TEXT NOT NULL DEFAULT '',"
            "pay_amount TEXT NOT NULL DEFAULT '0.00',"
            "refund_amount TEXT NOT NULL DEFAULT '0.00',"
            "refund_fee TEXT NOT NULL DEFAULT '0.00',"
            "reason TEXT NOT NULL DEFAULT '',"
            "channel_refund_no TEXT NOT NULL DEFAULT '',"
            "state INTEGER NOT NULL DEFAULT 0,"
            "created_at BIGINT NOT NULL DEFAULT 0,"
            "updated_at BIGINT NOT NULL DEFAULT 0)",

            "CREATE TABLE IF NOT EXISTS settle_order("
            "id BIGSERIAL PRIMARY KEY,"
            "settle_no TEXT NOT NULL UNIQUE,"
            "mch_id BIGINT NOT NULL,"
            "amount TEXT NOT NULL DEFAULT '0.00',"
            "fee TEXT NOT NULL DEFAULT '0.00',"
            "real_amount TEXT NOT NULL DEFAULT '0.00',"
            "account_info TEXT NOT NULL DEFAULT '',"
            "state INTEGER NOT NULL DEFAULT 0,"
            "admin_remark TEXT NOT NULL DEFAULT '',"
            "created_at BIGINT NOT NULL DEFAULT 0,"
            "updated_at BIGINT NOT NULL DEFAULT 0)",

            "CREATE TABLE IF NOT EXISTS money_log("
            "id BIGSERIAL PRIMARY KEY,"
            "mch_id BIGINT NOT NULL,"
            "change_type INTEGER NOT NULL,"
            "change_amount TEXT NOT NULL DEFAULT '0.00',"
            "before_amount TEXT NOT NULL DEFAULT '0.00',"
            "after_amount TEXT NOT NULL DEFAULT '0.00',"
            "biz_type TEXT NOT NULL DEFAULT '',"
            "biz_no TEXT NOT NULL DEFAULT '',"
            "remark TEXT NOT NULL DEFAULT '',"
            "created_at BIGINT NOT NULL DEFAULT 0)",

            "CREATE TABLE IF NOT EXISTS device("
            "id BIGSERIAL PRIMARY KEY,"
            "device_no TEXT NOT NULL UNIQUE,"
            "device_name TEXT NOT NULL DEFAULT '',"
            "device_type INTEGER NOT NULL DEFAULT 0,"
            "mch_id BIGINT NOT NULL DEFAULT 0,"
            "channel_id BIGINT NOT NULL DEFAULT 0,"
            "bind_account TEXT NOT NULL DEFAULT '',"
            "pay_types TEXT NOT NULL DEFAULT '',"
            "last_heart BIGINT NOT NULL DEFAULT 0,"
            "last_pay BIGINT NOT NULL DEFAULT 0,"
            "state INTEGER NOT NULL DEFAULT 0,"
            "extra_json TEXT NOT NULL DEFAULT '{}',"
            "created_at BIGINT NOT NULL DEFAULT 0,"
            "updated_at BIGINT NOT NULL DEFAULT 0)",

            "CREATE TABLE IF NOT EXISTS pay_qrcode("
            "id BIGSERIAL PRIMARY KEY,"
            "mch_id BIGINT NOT NULL DEFAULT 0,"
            "device_id BIGINT NOT NULL DEFAULT 0,"
            "type INTEGER NOT NULL,"
            "pay_url TEXT NOT NULL,"
            "price TEXT DEFAULT '0',"
            "state INTEGER NOT NULL DEFAULT 0,"
            "account TEXT NOT NULL DEFAULT '',"
            "pattern INTEGER NOT NULL DEFAULT 1,"
            "created_at BIGINT NOT NULL DEFAULT 0)",

            "CREATE TABLE IF NOT EXISTS tmp_price("
            "id BIGSERIAL PRIMARY KEY,"
            "price TEXT NOT NULL UNIQUE,"
            "oid TEXT NOT NULL)",

            "CREATE TABLE IF NOT EXISTS pay_notify_task("
            "id BIGSERIAL PRIMARY KEY,"
            "order_id TEXT NOT NULL UNIQUE,"
            "plugin TEXT NOT NULL DEFAULT '',"
            "notify_full_url TEXT NOT NULL,"
            "status INTEGER NOT NULL DEFAULT 0,"
            "retry_cnt INTEGER NOT NULL DEFAULT 0,"
            "next_retry_at BIGINT NOT NULL DEFAULT 0,"
            "last_response TEXT DEFAULT '',"
            "created_at BIGINT NOT NULL,"
            "updated_at BIGINT NOT NULL)",

            "CREATE TABLE IF NOT EXISTS pay_callback_log("
            "id BIGSERIAL PRIMARY KEY,"
            "order_id TEXT NOT NULL,"
            "notify_url TEXT DEFAULT '',"
            "http_status INTEGER NOT NULL DEFAULT 0,"
            "response TEXT DEFAULT '',"
            "success INTEGER NOT NULL DEFAULT 0,"
            "created_at BIGINT NOT NULL)",

            "CREATE TABLE IF NOT EXISTS oplog("
            "id BIGSERIAL PRIMARY KEY,"
            "user_type INTEGER NOT NULL DEFAULT 0,"
            "user_id BIGINT NOT NULL DEFAULT 0,"
            "username TEXT NOT NULL DEFAULT '',"
            "action TEXT NOT NULL DEFAULT '',"
            "target TEXT NOT NULL DEFAULT '',"
            "detail TEXT NOT NULL DEFAULT '',"
            "ip TEXT NOT NULL DEFAULT '',"
            "created_at BIGINT NOT NULL DEFAULT 0)",

            // ══════════════════════════════════════════════════════
            // v600~v602: 打印支持 (网页打印/热敏打印机/云打印)
            // ══════════════════════════════════════════════════════
            "CREATE TABLE IF NOT EXISTS print_task("
            "id BIGSERIAL PRIMARY KEY,"
            "task_id TEXT NOT NULL UNIQUE,"
            "printer_sn TEXT NOT NULL DEFAULT '',"
            "printer_key TEXT NOT NULL DEFAULT '',"
            "content TEXT NOT NULL DEFAULT '',"
            "copies INTEGER NOT NULL DEFAULT 1,"
            "order_id TEXT NOT NULL DEFAULT '',"
            "state INTEGER NOT NULL DEFAULT 0,"
            "error_msg TEXT NOT NULL DEFAULT '',"
            "created_at BIGINT NOT NULL DEFAULT 0,"
            "updated_at BIGINT NOT NULL DEFAULT 0)",

            "CREATE INDEX IF NOT EXISTS idx_ptask_state ON print_task(state)",
            "CREATE INDEX IF NOT EXISTS idx_ptask_order ON print_task(order_id)",
            "CREATE INDEX IF NOT EXISTS idx_ptask_created ON print_task(created_at)",

            "CREATE TABLE IF NOT EXISTS print_printer("
            "id BIGSERIAL PRIMARY KEY,"
            "name TEXT NOT NULL,"
            "sn TEXT NOT NULL UNIQUE,"
            "key TEXT NOT NULL DEFAULT '',"
            "brand TEXT NOT NULL DEFAULT 'feie',"
            "remark TEXT NOT NULL DEFAULT '',"
            "state INTEGER NOT NULL DEFAULT 1,"
            "created_at BIGINT NOT NULL DEFAULT 0,"
            "updated_at BIGINT NOT NULL DEFAULT 0)",

            "CREATE INDEX IF NOT EXISTS idx_pprinter_sn ON print_printer(sn)",
            "CREATE INDEX IF NOT EXISTS idx_pprinter_state ON print_printer(state)",

            "CREATE TABLE IF NOT EXISTS print_log("
            "id BIGSERIAL PRIMARY KEY,"
            "order_id TEXT NOT NULL DEFAULT '',"
            "print_type TEXT NOT NULL DEFAULT '',"
            "printer_sn TEXT NOT NULL DEFAULT '',"
            "copies INTEGER NOT NULL DEFAULT 1,"
            "result TEXT NOT NULL DEFAULT '',"
            "error_msg TEXT NOT NULL DEFAULT '',"
            "created_at BIGINT NOT NULL DEFAULT 0)",

            "CREATE INDEX IF NOT EXISTS idx_plog_order ON print_log(order_id)",
            "CREATE INDEX IF NOT EXISTS idx_plog_created ON print_log(created_at)",

            // ── 商户 Webhook 配置 ─────────────────────────────
            "CREATE TABLE IF NOT EXISTS mch_webhook("
            "id BIGSERIAL PRIMARY KEY,"
            "mch_id BIGINT NOT NULL,"
            "event_type TEXT NOT NULL DEFAULT '',"        // payment/refund/settle
            "notify_url TEXT NOT NULL DEFAULT '',"
            "secret_key TEXT NOT NULL DEFAULT '',"
            "sign_type TEXT NOT NULL DEFAULT 'HMAC-SHA256',"
            "state INTEGER NOT NULL DEFAULT 1,"
            "retry_count INTEGER NOT NULL DEFAULT 0,"
            "retry_max INTEGER NOT NULL DEFAULT 3,"
            "timeout_sec INTEGER NOT NULL DEFAULT 30,"
            "created_at BIGINT NOT NULL DEFAULT 0,"
            "updated_at BIGINT NOT NULL DEFAULT 0)",
            "CREATE INDEX IF NOT EXISTS idx_mch_wh_mch ON mch_webhook(mch_id, event_type, state)"
        };
    }
};
