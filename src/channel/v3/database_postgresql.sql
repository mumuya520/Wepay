-- WePay V3 PostgreSQL 数据库初始化脚本
-- 参考文档：实际建表由 src/common/DatabaseInit.h migration 440-449 完成
-- 表名前缀统一使用 v3_

CREATE DATABASE wepay_v3 WITH ENCODING 'UTF8';
\c wepay_v3;
CREATE EXTENSION IF NOT EXISTS "uuid-ossp";

-- 1. 设备状态表
CREATE TABLE IF NOT EXISTS v3_device (
    id BIGSERIAL PRIMARY KEY,
    device_id VARCHAR(64) NOT NULL UNIQUE,
    ip VARCHAR(64),
    last_heartbeat BIGINT NOT NULL DEFAULT 0,
    online SMALLINT DEFAULT 0,
    battery INTEGER DEFAULT -1,
    network VARCHAR(32),
    app_version VARCHAR(32),
    screen_resolution VARCHAR(32),
    created_at BIGINT NOT NULL DEFAULT 0,
    updated_at BIGINT NOT NULL DEFAULT 0
);
CREATE INDEX idx_v3dev_id ON v3_device(device_id);
CREATE INDEX idx_v3dev_online ON v3_device(online);
COMMENT ON TABLE v3_device IS '设备状态表';

-- 2. 设备-商户绑定表
CREATE TABLE IF NOT EXISTS v3_device_merchant (
    id BIGSERIAL PRIMARY KEY,
    device_id VARCHAR(64) NOT NULL,
    merchant_id VARCHAR(32) NOT NULL,
    bind_time BIGINT NOT NULL DEFAULT 0,
    status SMALLINT DEFAULT 1,
    created_at BIGINT NOT NULL DEFAULT 0,
    updated_at BIGINT NOT NULL DEFAULT 0,
    UNIQUE(device_id, merchant_id)
);
CREATE INDEX idx_v3dm_device ON v3_device_merchant(device_id);
CREATE INDEX idx_v3dm_merchant ON v3_device_merchant(merchant_id);
COMMENT ON TABLE v3_device_merchant IS '设备-商户绑定表';

-- 3. 订单主表
CREATE TABLE IF NOT EXISTS v3_order (
    id BIGSERIAL PRIMARY KEY,
    order_id VARCHAR(64) NOT NULL UNIQUE,
    merchant_order_id VARCHAR(64),
    merchant_id VARCHAR(32) NOT NULL,
    device_id VARCHAR(64),
    amount NUMERIC(10,2) NOT NULL,
    pay_type VARCHAR(32),
    status VARCHAR(32) DEFAULT 'PENDING',
    screenshot_url VARCHAR(500),
    idempotent_flag SMALLINT DEFAULT 0,
    notify_email VARCHAR(255),
    created_at BIGINT NOT NULL DEFAULT 0,
    pay_time BIGINT NOT NULL DEFAULT 0,
    expire_time BIGINT NOT NULL DEFAULT 0,
    updated_at BIGINT NOT NULL DEFAULT 0
);
CREATE INDEX idx_v3ord_id ON v3_order(order_id);
CREATE INDEX idx_v3ord_merchant ON v3_order(merchant_id);
CREATE INDEX idx_v3ord_device ON v3_order(device_id);
CREATE INDEX idx_v3ord_status ON v3_order(status);
COMMENT ON TABLE v3_order IS '订单主表';
COMMENT ON COLUMN v3_order.status IS 'PENDING/PUSHING/PAID/FAILED/TIMEOUT';

-- 4. 订单状态变更日志表
CREATE TABLE IF NOT EXISTS v3_order_status_log (
    id BIGSERIAL PRIMARY KEY,
    order_id VARCHAR(64) NOT NULL,
    old_status VARCHAR(32),
    new_status VARCHAR(32) NOT NULL,
    remark VARCHAR(500),
    created_at BIGINT NOT NULL DEFAULT 0
);
CREATE INDEX idx_v3osl_order ON v3_order_status_log(order_id);
COMMENT ON TABLE v3_order_status_log IS '订单状态变更日志表';

-- 5. 商户V3配置表
CREATE TABLE IF NOT EXISTS v3_merchant_config (
    id BIGSERIAL PRIMARY KEY,
    merchant_id VARCHAR(32) NOT NULL UNIQUE,
    merchant_name VARCHAR(128),
    hmac_secret VARCHAR(128) NOT NULL,
    rsa_public_key TEXT,
    callback_url VARCHAR(500),
    notify_email VARCHAR(255),
    email_notify_enabled SMALLINT DEFAULT 1,
    notify_on_success SMALLINT DEFAULT 1,
    notify_on_fail SMALLINT DEFAULT 1,
    daily_summary_enabled SMALLINT DEFAULT 1,
    status SMALLINT DEFAULT 1,
    created_at BIGINT NOT NULL DEFAULT 0,
    updated_at BIGINT NOT NULL DEFAULT 0
);
COMMENT ON TABLE v3_merchant_config IS '商户V3配置表';

-- 6. 系统配置表
CREATE TABLE IF NOT EXISTS v3_system_config (
    id BIGSERIAL PRIMARY KEY,
    config_key VARCHAR(128) NOT NULL UNIQUE,
    config_value TEXT,
    config_type VARCHAR(32) DEFAULT 'STRING',
    description VARCHAR(500),
    created_at BIGINT NOT NULL DEFAULT 0,
    updated_at BIGINT NOT NULL DEFAULT 0
);
INSERT INTO v3_system_config(config_key,config_value,config_type,description) VALUES
('timestamp_window','300','INT','时间戳防重放窗口（秒）'),
('max_devices','100','INT','最大设备数'),
('heartbeat_timeout','180','INT','心跳超时（秒）'),
('order_timeout','300','INT','订单超时（秒）'),
('enable_hmac','true','BOOL','启用HMAC签名'),
('enable_rsa','false','BOOL','启用RSA签名'),
('alert_enabled','false','BOOL','告警推送开关'),
('dingtalk_webhook','','STRING','钉钉Webhook地址'),
('wecom_webhook','','STRING','企业微信Webhook地址'),
('alert_email','','STRING','告警接收邮箱'),
('smtp_host','','STRING','SMTP服务器地址，如 smtp.qq.com'),
('smtp_port','465','STRING','SMTP端口，SSL通常465，TLS通常587'),
('smtp_username','','STRING','SMTP登录用户名（一般是发件邮箱）'),
('smtp_password','','STRING','SMTP登录密码或授权码'),
('smtp_from_email','','STRING','发件人邮箱地址'),
('smtp_from_name','WePay V3','STRING','发件人显示名称'),
('smtp_use_ssl','true','STRING','是否使用SSL（true/false）')
ON CONFLICT(config_key) DO NOTHING;
COMMENT ON TABLE v3_system_config IS '系统配置表';

-- 7. 安全审计日志表
CREATE TABLE IF NOT EXISTS v3_security_audit_log (
    id BIGSERIAL PRIMARY KEY,
    request_ip VARCHAR(64) NOT NULL,
    device_id VARCHAR(64),
    api_path VARCHAR(255) NOT NULL,
    request_method VARCHAR(16),
    sign_result SMALLINT,
    fail_reason VARCHAR(500),
    timestamp VARCHAR(32),
    nonce VARCHAR(128),
    created_at BIGINT NOT NULL DEFAULT 0
);
CREATE INDEX idx_v3sal_ip ON v3_security_audit_log(request_ip);
CREATE INDEX idx_v3sal_device ON v3_security_audit_log(device_id);
COMMENT ON TABLE v3_security_audit_log IS '安全审计日志表';

-- 8. 邮件发送记录表
CREATE TABLE IF NOT EXISTS v3_email_log (
    id BIGSERIAL PRIMARY KEY,
    order_id VARCHAR(64) NOT NULL,
    merchant_id VARCHAR(32) NOT NULL,
    email_to VARCHAR(255) NOT NULL,
    email_type VARCHAR(32) NOT NULL,
    subject VARCHAR(255) NOT NULL,
    content TEXT,
    send_status SMALLINT DEFAULT 0,
    send_time BIGINT NOT NULL DEFAULT 0,
    fail_reason VARCHAR(500),
    retry_count INTEGER DEFAULT 0,
    created_at BIGINT NOT NULL DEFAULT 0,
    updated_at BIGINT NOT NULL DEFAULT 0
);
CREATE INDEX idx_v3el_order ON v3_email_log(order_id);
CREATE INDEX idx_v3el_mch ON v3_email_log(merchant_id, created_at);
COMMENT ON TABLE v3_email_log IS '邮件发送记录表';

-- 9. 手动回调记录表
CREATE TABLE IF NOT EXISTS v3_manual_callback_log (
    id BIGSERIAL PRIMARY KEY,
    order_id VARCHAR(64) NOT NULL UNIQUE,
    merchant_id VARCHAR(32) NOT NULL,
    callback_url VARCHAR(500) NOT NULL,
    callback_token VARCHAR(128) NOT NULL,
    token_expire BIGINT NOT NULL DEFAULT 0,
    callback_status SMALLINT DEFAULT 0,
    callback_time BIGINT NOT NULL DEFAULT 0,
    callback_response TEXT,
    client_ip VARCHAR(64),
    user_agent VARCHAR(500),
    created_at BIGINT NOT NULL DEFAULT 0,
    updated_at BIGINT NOT NULL DEFAULT 0
);
CREATE INDEX idx_v3mcl_token ON v3_manual_callback_log(callback_token);
COMMENT ON TABLE v3_manual_callback_log IS '手动回调记录表';

-- 10. IP白名单表
CREATE TABLE IF NOT EXISTS v3_ip_whitelist (
    id BIGSERIAL PRIMARY KEY,
    merchant_id VARCHAR(32),
    ip_address VARCHAR(64) NOT NULL,
    description VARCHAR(255),
    status SMALLINT DEFAULT 1,
    created_at BIGINT NOT NULL DEFAULT 0,
    updated_at BIGINT NOT NULL DEFAULT 0
);
CREATE INDEX idx_v3iw_ip ON v3_ip_whitelist(ip_address);
COMMENT ON TABLE v3_ip_whitelist IS 'IP白名单表';