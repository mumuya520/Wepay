-- WePay V3 MySQL 数据库初始化脚本
-- 参考文档：实际建表由 src/common/DatabaseInit.h migration 440-449 完成
-- 表名前缀统一使用 v3_

CREATE DATABASE IF NOT EXISTS wepay_v3 DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE wepay_v3;

-- 1. 设备状态表
CREATE TABLE IF NOT EXISTS v3_device (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    device_id VARCHAR(64) NOT NULL UNIQUE COMMENT '设备唯一ID',
    ip VARCHAR(64) COMMENT '设备IP地址',
    last_heartbeat INTEGER NOT NULL DEFAULT 0 COMMENT '最后心跳时间戳(Unix秒)',
    online TINYINT DEFAULT 0 COMMENT '在线状态：0离线 1在线',
    battery INTEGER DEFAULT -1 COMMENT '电量百分比',
    network VARCHAR(32) COMMENT '网络类型：WiFi/4G/5G',
    app_version VARCHAR(32) COMMENT 'App版本',
    screen_resolution VARCHAR(32) COMMENT '屏幕分辨率',
    created_at INTEGER NOT NULL DEFAULT 0,
    updated_at INTEGER NOT NULL DEFAULT 0,
    INDEX idx_v3dev_id (device_id),
    INDEX idx_v3dev_online (online)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='设备状态表';

-- 2. 设备-商户绑定表
CREATE TABLE IF NOT EXISTS v3_device_merchant (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    device_id VARCHAR(64) NOT NULL COMMENT '设备ID',
    merchant_id VARCHAR(32) NOT NULL COMMENT '商户ID',
    bind_time INTEGER NOT NULL DEFAULT 0 COMMENT '绑定时间戳',
    status TINYINT DEFAULT 1 COMMENT '状态：0已解绑 1已绑定',
    created_at INTEGER NOT NULL DEFAULT 0,
    updated_at INTEGER NOT NULL DEFAULT 0,
    UNIQUE KEY uk_device_merchant (device_id, merchant_id),
    INDEX idx_v3dm_device (device_id),
    INDEX idx_v3dm_merchant (merchant_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='设备-商户绑定表';

-- 3. 订单主表
CREATE TABLE IF NOT EXISTS v3_order (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    order_id VARCHAR(64) NOT NULL UNIQUE COMMENT '订单号',
    merchant_order_id VARCHAR(64) COMMENT '商户订单号',
    merchant_id VARCHAR(32) NOT NULL COMMENT '商户ID',
    device_id VARCHAR(64) COMMENT '设备ID',
    amount DECIMAL(10,2) NOT NULL COMMENT '订单金额',
    pay_type VARCHAR(32) COMMENT '支付类型：WECHAT/ALIPAY/QQ',
    status VARCHAR(32) DEFAULT 'PENDING' COMMENT '状态：PENDING/PUSHING/PAID/FAILED/TIMEOUT',
    screenshot_url VARCHAR(500) COMMENT '截图URL',
    idempotent_flag TINYINT DEFAULT 0 COMMENT '幂等标记',
    notify_email VARCHAR(255) COMMENT '通知邮箱',
    created_at INTEGER NOT NULL DEFAULT 0,
    pay_time INTEGER NOT NULL DEFAULT 0,
    expire_time INTEGER NOT NULL DEFAULT 0,
    updated_at INTEGER NOT NULL DEFAULT 0,
    INDEX idx_v3ord_id (order_id),
    INDEX idx_v3ord_merchant (merchant_id),
    INDEX idx_v3ord_device (device_id),
    INDEX idx_v3ord_status (status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='订单主表';

-- 4. 订单状态变更日志表
CREATE TABLE IF NOT EXISTS v3_order_status_log (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    order_id VARCHAR(64) NOT NULL COMMENT '订单号',
    old_status VARCHAR(32) COMMENT '旧状态',
    new_status VARCHAR(32) NOT NULL COMMENT '新状态',
    remark VARCHAR(500) COMMENT '备注（来源说明）',
    created_at INTEGER NOT NULL DEFAULT 0,
    INDEX idx_v3osl_order (order_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='订单状态变更日志表';

-- 5. 商户V3配置表
CREATE TABLE IF NOT EXISTS v3_merchant_config (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    merchant_id VARCHAR(32) NOT NULL UNIQUE COMMENT '商户ID',
    merchant_name VARCHAR(128) COMMENT '商户名称',
    hmac_secret VARCHAR(128) NOT NULL COMMENT 'HMAC密钥',
    rsa_public_key TEXT COMMENT 'RSA公钥（可选）',
    callback_url VARCHAR(500) COMMENT '回调地址',
    notify_email VARCHAR(255) COMMENT '通知邮箱',
    email_notify_enabled TINYINT DEFAULT 1,
    notify_on_success TINYINT DEFAULT 1,
    notify_on_fail TINYINT DEFAULT 1,
    daily_summary_enabled TINYINT DEFAULT 1,
    status TINYINT DEFAULT 1 COMMENT '状态：0禁用 1启用',
    created_at INTEGER NOT NULL DEFAULT 0,
    updated_at INTEGER NOT NULL DEFAULT 0
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='商户V3配置表';

-- 6. 系统配置表
CREATE TABLE IF NOT EXISTS v3_system_config (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    config_key VARCHAR(128) NOT NULL UNIQUE COMMENT '配置键',
    config_value TEXT COMMENT '配置值',
    config_type VARCHAR(32) DEFAULT 'STRING' COMMENT 'STRING/INT/BOOL/JSON',
    description VARCHAR(500) COMMENT '说明',
    created_at INTEGER NOT NULL DEFAULT 0,
    updated_at INTEGER NOT NULL DEFAULT 0
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='系统配置表';

-- 6.1 默认系统配置
INSERT IGNORE INTO v3_system_config(config_key,config_value,config_type,description) VALUES
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
('smtp_use_ssl','true','STRING','是否使用SSL（true/false）');

-- 7. 安全审计日志表
CREATE TABLE IF NOT EXISTS v3_security_audit_log (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    request_ip VARCHAR(64) NOT NULL COMMENT '请求IP',
    device_id VARCHAR(64) COMMENT '设备ID',
    api_path VARCHAR(255) NOT NULL COMMENT '接口路径',
    request_method VARCHAR(16) COMMENT '请求方法',
    sign_result TINYINT COMMENT '签名结果：0失败 1成功',
    fail_reason VARCHAR(500) COMMENT '失败原因',
    timestamp VARCHAR(32) COMMENT '请求时间戳',
    nonce VARCHAR(128) COMMENT 'Nonce',
    created_at INTEGER NOT NULL DEFAULT 0,
    INDEX idx_v3sal_ip (request_ip),
    INDEX idx_v3sal_device (device_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='安全审计日志表';

-- 8. 邮件发送记录表
CREATE TABLE IF NOT EXISTS v3_email_log (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    order_id VARCHAR(64) NOT NULL COMMENT '订单号',
    merchant_id VARCHAR(32) NOT NULL COMMENT '商户ID',
    email_to VARCHAR(255) NOT NULL COMMENT '收件人',
    email_type VARCHAR(32) NOT NULL COMMENT '邮件类型：PAY_SUCCESS/PAY_FAIL/TIMEOUT/CALLBACK_FAIL',
    subject VARCHAR(255) NOT NULL,
    content TEXT,
    send_status TINYINT DEFAULT 0 COMMENT '0待发送 1成功 2失败',
    send_time INTEGER NOT NULL DEFAULT 0,
    fail_reason VARCHAR(500),
    retry_count INTEGER DEFAULT 0,
    created_at INTEGER NOT NULL DEFAULT 0,
    updated_at INTEGER NOT NULL DEFAULT 0,
    INDEX idx_v3el_order (order_id),
    INDEX idx_v3el_mch (merchant_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='邮件发送记录表';

-- 9. 手动回调记录表
CREATE TABLE IF NOT EXISTS v3_manual_callback_log (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    order_id VARCHAR(64) NOT NULL UNIQUE COMMENT '订单号',
    merchant_id VARCHAR(32) NOT NULL,
    callback_url VARCHAR(500) NOT NULL,
    callback_token VARCHAR(128) NOT NULL,
    token_expire INTEGER NOT NULL DEFAULT 0 COMMENT '令牌过期时间戳',
    callback_status TINYINT DEFAULT 0 COMMENT '0待回调 1成功 2失败',
    callback_time INTEGER NOT NULL DEFAULT 0,
    callback_response TEXT,
    client_ip VARCHAR(64),
    user_agent VARCHAR(500),
    created_at INTEGER NOT NULL DEFAULT 0,
    updated_at INTEGER NOT NULL DEFAULT 0,
    INDEX idx_v3mcl_token (callback_token)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='手动回调记录表';

-- 10. IP白名单表
CREATE TABLE IF NOT EXISTS v3_ip_whitelist (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    merchant_id VARCHAR(32) COMMENT '商户ID（为空表示全局白名单）',
    ip_address VARCHAR(64) NOT NULL COMMENT 'IP地址',
    description VARCHAR(255) COMMENT '备注',
    status TINYINT DEFAULT 1 COMMENT '0禁用 1启用',
    created_at INTEGER NOT NULL DEFAULT 0,
    updated_at INTEGER NOT NULL DEFAULT 0,
    INDEX idx_v3iw_ip (ip_address)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='IP白名单表';