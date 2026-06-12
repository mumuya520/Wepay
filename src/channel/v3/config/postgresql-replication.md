# PostgreSQL 主从复制配置指南

## 架构说明
- **主库（Master）**: 192.168.1.10:5432 - 处理所有写操作
- **从库（Slave）**: 192.168.1.11:5432 - 处理读操作，实时同步主库数据
- **复制方式**: 流复制（Streaming Replication）
- **同步模式**: 异步复制（可选同步复制）

---

## 一、主库（Master）配置

### 1. 修改 postgresql.conf

```bash
# 编辑配置文件
sudo vim /etc/postgresql/14/main/postgresql.conf
```

添加或修改以下配置：

```conf
# 监听地址
listen_addresses = '*'

# WAL日志级别（必须为replica或logical）
wal_level = replica

# 最大WAL发送进程数（至少等于从库数量）
max_wal_senders = 10

# WAL保留大小（防止从库延迟时主库删除WAL）
wal_keep_size = 1GB

# 归档模式（可选，用于PITR恢复）
archive_mode = on
archive_command = 'test ! -f /var/lib/postgresql/archive/%f && cp %p /var/lib/postgresql/archive/%f'

# 同步复制（可选，确保数据不丢失）
# synchronous_commit = on
# synchronous_standby_names = 'slave1'

# 最大复制槽数量
max_replication_slots = 10

# 热备份（允许从库查询）
hot_standby = on
```

### 2. 修改 pg_hba.conf

```bash
# 编辑访问控制文件
sudo vim /etc/postgresql/14/main/pg_hba.conf
```

添加从库复制权限：

```conf
# 允许从库连接进行复制
# TYPE  DATABASE        USER            ADDRESS                 METHOD
host    replication     replicator      192.168.1.11/32         md5
host    replication     replicator      192.168.1.0/24          md5
```

### 3. 创建复制用户

```sql
-- 连接到主库
psql -U postgres

-- 创建复制用户
CREATE USER replicator WITH REPLICATION ENCRYPTED PASSWORD 'replicator_password_123';

-- 授予必要权限
GRANT CONNECT ON DATABASE wepay_v3 TO replicator;
```

### 4. 创建复制槽（可选，推荐）

```sql
-- 创建复制槽，防止WAL被过早删除
SELECT * FROM pg_create_physical_replication_slot('slave1_slot');

-- 查看复制槽
SELECT * FROM pg_replication_slots;
```

### 5. 重启主库

```bash
sudo systemctl restart postgresql
```

---

## 二、从库（Slave）配置

### 1. 停止从库服务

```bash
sudo systemctl stop postgresql
```

### 2. 清空从库数据目录

```bash
# 备份原有数据（如果需要）
sudo mv /var/lib/postgresql/14/main /var/lib/postgresql/14/main.bak

# 创建新的数据目录
sudo mkdir -p /var/lib/postgresql/14/main
sudo chown postgres:postgres /var/lib/postgresql/14/main
```

### 3. 使用 pg_basebackup 初始化从库

```bash
# 切换到postgres用户
sudo su - postgres

# 从主库复制数据
pg_basebackup -h 192.168.1.10 -U replicator -D /var/lib/postgresql/14/main \
    -P -v -R -X stream -C -S slave1_slot

# 参数说明：
# -h: 主库地址
# -U: 复制用户
# -D: 从库数据目录
# -P: 显示进度
# -v: 详细输出
# -R: 自动创建 standby.signal 和 postgresql.auto.conf
# -X stream: 流式传输WAL
# -C: 自动创建复制槽
# -S: 指定复制槽名称
```

### 4. 修改从库 postgresql.conf（可选）

```bash
sudo vim /var/lib/postgresql/14/main/postgresql.conf
```

```conf
# 热备份（允许从库查询）
hot_standby = on

# 从库反馈（用于同步复制）
hot_standby_feedback = on

# 最大备用延迟（可选）
max_standby_streaming_delay = 30s
```

### 5. 检查 postgresql.auto.conf

pg_basebackup 会自动创建此文件，内容类似：

```conf
# 主库连接信息
primary_conninfo = 'host=192.168.1.10 port=5432 user=replicator password=replicator_password_123'

# 复制槽名称
primary_slot_name = 'slave1_slot'
```

### 6. 启动从库

```bash
sudo systemctl start postgresql
```

---

## 三、验证复制状态

### 1. 在主库查看复制状态

```sql
-- 查看复制进程
SELECT * FROM pg_stat_replication;

-- 查看复制槽
SELECT * FROM pg_replication_slots;

-- 查看WAL发送状态
SELECT 
    client_addr,
    state,
    sent_lsn,
    write_lsn,
    flush_lsn,
    replay_lsn,
    sync_state
FROM pg_stat_replication;
```

### 2. 在从库查看复制状态

```sql
-- 查看从库状态
SELECT * FROM pg_stat_wal_receiver;

-- 检查是否为从库
SELECT pg_is_in_recovery();  -- 返回 t 表示是从库

-- 查看复制延迟
SELECT 
    now() - pg_last_xact_replay_timestamp() AS replication_delay;
```

### 3. 测试数据同步

```sql
-- 在主库创建测试表
CREATE TABLE replication_test (id SERIAL PRIMARY KEY, data TEXT);
INSERT INTO replication_test (data) VALUES ('test data');

-- 在从库查询（应该能看到数据）
SELECT * FROM replication_test;
```

---

## 四、读写分离配置

### 应用层配置（C++代码）

```cpp
// 主库连接（写操作）
auto masterDb = drogon::app().getDbClient("master");

// 从库连接（读操作）
auto slaveDb = drogon::app().getDbClient("slave");

// 写操作使用主库
masterDb->execSqlAsync(
    "INSERT INTO orders (order_id, amount) VALUES ($1, $2)",
    [](const Result& r) { /* ... */ },
    [](const DrogonDbException& e) { /* ... */ },
    orderId, amount
);

// 读操作使用从库
slaveDb->execSqlAsync(
    "SELECT * FROM orders WHERE merchant_id = $1",
    [](const Result& r) { /* ... */ },
    [](const DrogonDbException& e) { /* ... */ },
    merchantId
);
```

### Drogon 配置文件

```json
{
  "db_clients": [
    {
      "name": "master",
      "rdbms": "postgresql",
      "host": "192.168.1.10",
      "port": 5432,
      "dbname": "wepay_v3",
      "user": "wepay_user",
      "passwd": "wepay_password",
      "is_fast": true,
      "connection_number": 10
    },
    {
      "name": "slave",
      "rdbms": "postgresql",
      "host": "192.168.1.11",
      "port": 5432,
      "dbname": "wepay_v3",
      "user": "wepay_user",
      "passwd": "wepay_password",
      "is_fast": true,
      "connection_number": 20
    }
  ]
}
```

---

## 五、故障切换（Failover）

### 手动提升从库为主库

```bash
# 在从库执行
sudo su - postgres
pg_ctl promote -D /var/lib/postgresql/14/main

# 或者创建触发文件
touch /var/lib/postgresql/14/main/promote.trigger
```

### 自动故障切换（使用 Patroni）

推荐使用 Patroni + etcd/Consul 实现自动故障切换。

---

## 六、监控指标

### 关键监控指标

1. **复制延迟**: `pg_stat_replication.replay_lag`
2. **WAL发送速率**: `pg_stat_replication.sent_lsn`
3. **复制槽状态**: `pg_replication_slots.active`
4. **从库连接状态**: `pg_stat_wal_receiver.status`

### 告警阈值

- 复制延迟 > 10秒：警告
- 复制延迟 > 60秒：严重
- 从库断开连接：严重
- WAL堆积 > 5GB：警告

---

## 七、常见问题

### 1. 从库延迟过大

```sql
-- 检查主库负载
SELECT * FROM pg_stat_activity WHERE state = 'active';

-- 检查从库是否有长事务
SELECT * FROM pg_stat_activity WHERE state = 'idle in transaction';

-- 增加 wal_keep_size
ALTER SYSTEM SET wal_keep_size = '2GB';
SELECT pg_reload_conf();
```

### 2. 复制中断

```bash
# 检查网络连接
ping 192.168.1.10

# 检查复制用户权限
psql -h 192.168.1.10 -U replicator -d postgres

# 重新初始化从库
pg_basebackup -h 192.168.1.10 -U replicator -D /var/lib/postgresql/14/main -P -v -R
```

### 3. WAL文件堆积

```sql
-- 检查复制槽状态
SELECT slot_name, active, restart_lsn FROM pg_replication_slots;

-- 删除不活跃的复制槽
SELECT pg_drop_replication_slot('inactive_slot');
```

---

## 八、备份策略

### 1. 主库备份（每日）

```bash
#!/bin/bash
# 全量备份脚本
BACKUP_DIR="/backup/postgresql"
DATE=$(date +%Y%m%d)

pg_basebackup -h 192.168.1.10 -U postgres -D ${BACKUP_DIR}/${DATE} -F tar -z -P

# 保留最近7天的备份
find ${BACKUP_DIR} -type d -mtime +7 -exec rm -rf {} \;
```

### 2. WAL归档备份

```bash
# 归档目录
mkdir -p /var/lib/postgresql/archive

# 定期同步到远程存储
rsync -avz /var/lib/postgresql/archive/ backup-server:/backup/wepay/wal/
```

---

## 九、性能优化

### 主库优化

```conf
# 增加WAL缓冲区
wal_buffers = 16MB

# 增加检查点间隔
checkpoint_timeout = 15min
max_wal_size = 2GB

# 异步提交（可选，提高性能但可能丢失少量数据）
synchronous_commit = off
```

### 从库优化

```conf
# 增加从库查询并发
max_connections = 200

# 优化查询缓存
shared_buffers = 4GB
effective_cache_size = 12GB
```

---

## 十、安全建议

1. **使用SSL连接**: 在 `primary_conninfo` 中添加 `sslmode=require`
2. **定期更换复制用户密码**
3. **限制复制用户权限**: 仅授予 REPLICATION 权限
4. **监控复制状态**: 设置告警通知
5. **定期测试故障切换**: 确保切换流程正常

---

## 配置文件位置

- 主库配置: `/etc/postgresql/14/main/postgresql.conf`
- 从库配置: `/var/lib/postgresql/14/main/postgresql.auto.conf`
- 访问控制: `/etc/postgresql/14/main/pg_hba.conf`
- 备份脚本: `/usr/local/bin/pg_backup.sh`
