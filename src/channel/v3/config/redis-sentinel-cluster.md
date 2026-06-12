# Redis 哨兵/集群配置指南

## 架构选择

### 方案一：Redis Sentinel（哨兵模式）
- **适用场景**: 中小规模，单主多从，自动故障转移
- **优点**: 配置简单，自动故障切换，读写分离
- **缺点**: 单主写入，无法水平扩展

### 方案二：Redis Cluster（集群模式）
- **适用场景**: 大规模，数据分片，水平扩展
- **优点**: 数据分片，高可用，水平扩展
- **缺点**: 配置复杂，不支持多数据库

**推荐**: WePay V3 使用 **Redis Sentinel** 模式（3主3从6哨兵）

---

## 一、Redis Sentinel 配置（推荐）

### 架构说明
- **主节点（Master）**: 192.168.1.20:6379
- **从节点1（Slave1）**: 192.168.1.21:6379
- **从节点2（Slave2）**: 192.168.1.22:6379
- **哨兵节点**: 每台机器运行一个哨兵（26379端口）

---

### 1. 主节点配置（192.168.1.20）

#### redis.conf

```bash
# 编辑配置文件
sudo vim /etc/redis/redis.conf
```

```conf
# 绑定地址
bind 0.0.0.0

# 端口
port 6379

# 守护进程
daemonize yes

# PID文件
pidfile /var/run/redis_6379.pid

# 日志文件
logfile /var/log/redis/redis-server.log

# 数据目录
dir /var/lib/redis

# 持久化配置
# RDB快照
save 900 1
save 300 10
save 60 10000
dbfilename dump.rdb

# AOF持久化
appendonly yes
appendfilename "appendonly.aof"
appendfsync everysec

# 内存配置
maxmemory 4gb
maxmemory-policy allkeys-lru

# 密码认证
requirepass wepay_redis_password_123

# 主从复制密码
masterauth wepay_redis_password_123

# 慢查询日志
slowlog-log-slower-than 10000
slowlog-max-len 128

# 客户端连接数
maxclients 10000

# 超时时间
timeout 300

# TCP keepalive
tcp-keepalive 300

# 保护模式（关闭，允许远程连接）
protected-mode no
```

---

### 2. 从节点配置（192.168.1.21, 192.168.1.22）

#### redis.conf

```conf
# 基础配置同主节点
bind 0.0.0.0
port 6379
daemonize yes
pidfile /var/run/redis_6379.pid
logfile /var/log/redis/redis-server.log
dir /var/lib/redis

# 持久化配置
save 900 1
save 300 10
save 60 10000
dbfilename dump.rdb
appendonly yes
appendfilename "appendonly.aof"
appendfsync everysec

# 内存配置
maxmemory 4gb
maxmemory-policy allkeys-lru

# 密码认证
requirepass wepay_redis_password_123
masterauth wepay_redis_password_123

# 主从复制配置（关键）
replicaof 192.168.1.20 6379

# 从节点只读
replica-read-only yes

# 从节点优先级（数字越小优先级越高）
replica-priority 100

# 保护模式
protected-mode no
```

---

### 3. 哨兵配置（所有节点）

#### sentinel.conf

```bash
# 创建哨兵配置文件
sudo vim /etc/redis/sentinel.conf
```

```conf
# 绑定地址
bind 0.0.0.0

# 端口
port 26379

# 守护进程
daemonize yes

# PID文件
pidfile /var/run/redis-sentinel.pid

# 日志文件
logfile /var/log/redis/sentinel.log

# 工作目录
dir /var/lib/redis

# 监控主节点
# sentinel monitor <master-name> <ip> <port> <quorum>
# quorum: 判定主节点下线需要的哨兵数量
sentinel monitor wepay-master 192.168.1.20 6379 2

# 主节点密码
sentinel auth-pass wepay-master wepay_redis_password_123

# 主节点下线判定时间（毫秒）
sentinel down-after-milliseconds wepay-master 5000

# 故障转移超时时间（毫秒）
sentinel failover-timeout wepay-master 60000

# 同时进行复制的从节点数量
sentinel parallel-syncs wepay-master 1

# 哨兵通知脚本（可选）
# sentinel notification-script wepay-master /usr/local/bin/redis-notify.sh

# 故障转移脚本（可选）
# sentinel client-reconfig-script wepay-master /usr/local/bin/redis-failover.sh
```

---

### 4. 启动服务

#### 启动 Redis 主节点

```bash
# 主节点（192.168.1.20）
sudo systemctl start redis-server
sudo systemctl enable redis-server

# 验证
redis-cli -a wepay_redis_password_123 ping
```

#### 启动 Redis 从节点

```bash
# 从节点1（192.168.1.21）
sudo systemctl start redis-server
sudo systemctl enable redis-server

# 从节点2（192.168.1.22）
sudo systemctl start redis-server
sudo systemctl enable redis-server

# 验证主从复制
redis-cli -a wepay_redis_password_123 INFO replication
```

#### 启动哨兵（所有节点）

```bash
# 在所有3台机器上启动哨兵
sudo redis-sentinel /etc/redis/sentinel.conf

# 或使用systemd
sudo systemctl start redis-sentinel
sudo systemctl enable redis-sentinel

# 验证哨兵状态
redis-cli -p 26379 SENTINEL masters
redis-cli -p 26379 SENTINEL slaves wepay-master
redis-cli -p 26379 SENTINEL sentinels wepay-master
```

---

### 5. 应用层配置（C++代码）

#### 使用 redis++ 连接哨兵

```cpp
#include <sw/redis++/redis++.h>

using namespace sw::redis;

// 哨兵配置
SentinelOptions sentinel_opts;
sentinel_opts.nodes = {
    {"192.168.1.20", 26379},
    {"192.168.1.21", 26379},
    {"192.168.1.22", 26379}
};
sentinel_opts.connect_timeout = std::chrono::milliseconds(100);
sentinel_opts.socket_timeout = std::chrono::milliseconds(100);

// 连接选项
ConnectionOptions conn_opts;
conn_opts.password = "wepay_redis_password_123";
conn_opts.connect_timeout = std::chrono::milliseconds(100);
conn_opts.socket_timeout = std::chrono::milliseconds(100);

// 连接池选项
ConnectionPoolOptions pool_opts;
pool_opts.size = 10;
pool_opts.wait_timeout = std::chrono::milliseconds(100);

// 创建 Sentinel 对象
auto sentinel = std::make_shared<Sentinel>(sentinel_opts);

// 创建 Redis 对象（自动连接主节点）
auto redis = std::make_shared<Redis>(
    sentinel,
    "wepay-master",  // master name
    Role::MASTER,    // 连接主节点（写操作）
    conn_opts,
    pool_opts
);

// 读操作可以连接从节点
auto redis_slave = std::make_shared<Redis>(
    sentinel,
    "wepay-master",
    Role::SLAVE,     // 连接从节点（读操作）
    conn_opts,
    pool_opts
);

// 使用示例
redis->set("key", "value");
auto val = redis_slave->get("key");
```

#### WepayV3Config.cpp 配置

```cpp
// 初始化 Redis Sentinel
void WepayV3Config::initRedisSentinel() {
    SentinelOptions sentinel_opts;
    for (const auto& node : redis.sentinelNodes) {
        sentinel_opts.nodes.push_back({node.host, node.port});
    }
    
    ConnectionOptions conn_opts;
    conn_opts.password = redis.password;
    conn_opts.db = redis.db;
    
    ConnectionPoolOptions pool_opts;
    pool_opts.size = redis.poolSize;
    
    sentinel_ = std::make_shared<Sentinel>(sentinel_opts);
    
    // 主节点连接（写操作）
    redisMaster_ = std::make_shared<Redis>(
        sentinel_,
        redis.masterName,
        Role::MASTER,
        conn_opts,
        pool_opts
    );
    
    // 从节点连接（读操作）
    redisSlave_ = std::make_shared<Redis>(
        sentinel_,
        redis.masterName,
        Role::SLAVE,
        conn_opts,
        pool_opts
    );
}
```

---

### 6. 验证哨兵功能

#### 查看主节点信息

```bash
redis-cli -p 26379 SENTINEL get-master-addr-by-name wepay-master
```

#### 查看从节点信息

```bash
redis-cli -p 26379 SENTINEL slaves wepay-master
```

#### 测试故障转移

```bash
# 停止主节点
sudo systemctl stop redis-server

# 观察哨兵日志（应该自动选举新主节点）
tail -f /var/log/redis/sentinel.log

# 查看新主节点
redis-cli -p 26379 SENTINEL get-master-addr-by-name wepay-master
```

---

## 二、Redis Cluster 配置（可选）

### 架构说明
- **6节点集群**: 3主3从
- **数据分片**: 16384个槽位
- **自动故障转移**: 无需哨兵

### 1. 节点配置

#### redis.conf（所有节点）

```conf
# 基础配置
bind 0.0.0.0
port 6379
daemonize yes
pidfile /var/run/redis_6379.pid
logfile /var/log/redis/redis-cluster.log
dir /var/lib/redis

# 集群配置
cluster-enabled yes
cluster-config-file nodes-6379.conf
cluster-node-timeout 5000
cluster-require-full-coverage no

# 密码认证
requirepass wepay_redis_password_123
masterauth wepay_redis_password_123

# 持久化
appendonly yes
appendfilename "appendonly.aof"
```

### 2. 创建集群

```bash
# 使用 redis-cli 创建集群
redis-cli --cluster create \
    192.168.1.20:6379 \
    192.168.1.21:6379 \
    192.168.1.22:6379 \
    192.168.1.23:6379 \
    192.168.1.24:6379 \
    192.168.1.25:6379 \
    --cluster-replicas 1 \
    -a wepay_redis_password_123

# 验证集群状态
redis-cli -c -a wepay_redis_password_123 CLUSTER INFO
redis-cli -c -a wepay_redis_password_123 CLUSTER NODES
```

### 3. 应用层配置

```cpp
// Redis Cluster 连接
ConnectionOptions conn_opts;
conn_opts.password = "wepay_redis_password_123";

ConnectionPoolOptions pool_opts;
pool_opts.size = 10;

// 集群节点列表
std::vector<std::pair<std::string, int>> cluster_nodes = {
    {"192.168.1.20", 6379},
    {"192.168.1.21", 6379},
    {"192.168.1.22", 6379},
    {"192.168.1.23", 6379},
    {"192.168.1.24", 6379},
    {"192.168.1.25", 6379}
};

// 创建 RedisCluster 对象
auto redis_cluster = std::make_shared<RedisCluster>(
    cluster_nodes.begin(),
    cluster_nodes.end(),
    conn_opts,
    pool_opts
);

// 使用示例
redis_cluster->set("key", "value");
auto val = redis_cluster->get("key");
```

---

## 三、监控与告警

### 1. 关键监控指标

```bash
# 内存使用
redis-cli -a wepay_redis_password_123 INFO memory | grep used_memory_human

# 连接数
redis-cli -a wepay_redis_password_123 INFO clients | grep connected_clients

# 命令统计
redis-cli -a wepay_redis_password_123 INFO stats | grep instantaneous_ops_per_sec

# 主从复制延迟
redis-cli -a wepay_redis_password_123 INFO replication | grep master_repl_offset

# 慢查询
redis-cli -a wepay_redis_password_123 SLOWLOG GET 10
```

### 2. 监控脚本

```bash
#!/bin/bash
# redis-monitor.sh

REDIS_CLI="redis-cli -a wepay_redis_password_123"

# 检查 Redis 是否运行
if ! $REDIS_CLI ping > /dev/null 2>&1; then
    echo "CRITICAL: Redis is down"
    exit 2
fi

# 检查内存使用
MEMORY_USED=$($REDIS_CLI INFO memory | grep used_memory_human | awk -F: '{print $2}' | tr -d '\r')
echo "Memory used: $MEMORY_USED"

# 检查连接数
CLIENTS=$($REDIS_CLI INFO clients | grep connected_clients | awk -F: '{print $2}' | tr -d '\r')
echo "Connected clients: $CLIENTS"

# 检查主从复制状态
ROLE=$($REDIS_CLI INFO replication | grep role | awk -F: '{print $2}' | tr -d '\r')
echo "Role: $ROLE"

if [ "$ROLE" == "slave" ]; then
    MASTER_LINK=$($REDIS_CLI INFO replication | grep master_link_status | awk -F: '{print $2}' | tr -d '\r')
    echo "Master link status: $MASTER_LINK"
    
    if [ "$MASTER_LINK" != "up" ]; then
        echo "WARNING: Master link is down"
        exit 1
    fi
fi

echo "OK: Redis is healthy"
exit 0
```

---

## 四、备份与恢复

### 1. RDB 备份

```bash
#!/bin/bash
# redis-backup.sh

BACKUP_DIR="/backup/redis"
DATE=$(date +%Y%m%d_%H%M%S)

# 触发 BGSAVE
redis-cli -a wepay_redis_password_123 BGSAVE

# 等待备份完成
while [ $(redis-cli -a wepay_redis_password_123 LASTSAVE) -eq $(redis-cli -a wepay_redis_password_123 LASTSAVE) ]; do
    sleep 1
done

# 复制 RDB 文件
cp /var/lib/redis/dump.rdb ${BACKUP_DIR}/dump_${DATE}.rdb

# 保留最近7天的备份
find ${BACKUP_DIR} -name "dump_*.rdb" -mtime +7 -delete
```

### 2. AOF 备份

```bash
# 复制 AOF 文件
cp /var/lib/redis/appendonly.aof ${BACKUP_DIR}/appendonly_${DATE}.aof
```

### 3. 恢复数据

```bash
# 停止 Redis
sudo systemctl stop redis-server

# 恢复 RDB 文件
cp ${BACKUP_DIR}/dump_20240123.rdb /var/lib/redis/dump.rdb

# 或恢复 AOF 文件
cp ${BACKUP_DIR}/appendonly_20240123.aof /var/lib/redis/appendonly.aof

# 修改权限
chown redis:redis /var/lib/redis/*.rdb
chown redis:redis /var/lib/redis/*.aof

# 启动 Redis
sudo systemctl start redis-server
```

---

## 五、性能优化

### 1. 内存优化

```conf
# 内存淘汰策略
maxmemory-policy allkeys-lru

# 压缩配置
hash-max-ziplist-entries 512
hash-max-ziplist-value 64
list-max-ziplist-size -2
set-max-intset-entries 512
zset-max-ziplist-entries 128
zset-max-ziplist-value 64
```

### 2. 持久化优化

```conf
# AOF 重写
auto-aof-rewrite-percentage 100
auto-aof-rewrite-min-size 64mb

# RDB 压缩
rdbcompression yes
rdbchecksum yes
```

### 3. 网络优化

```conf
# TCP backlog
tcp-backlog 511

# 禁用 Nagle 算法
tcp-nodelay yes
```

---

## 六、安全加固

### 1. 密码认证

```conf
requirepass wepay_redis_password_123
```

### 2. 命令重命名

```conf
# 重命名危险命令
rename-command FLUSHDB ""
rename-command FLUSHALL ""
rename-command CONFIG "CONFIG_wepay_secret"
rename-command SHUTDOWN "SHUTDOWN_wepay_secret"
```

### 3. 防火墙规则

```bash
# 仅允许内网访问
sudo ufw allow from 192.168.1.0/24 to any port 6379
sudo ufw allow from 192.168.1.0/24 to any port 26379
```

---

## 配置文件位置

- Redis 配置: `/etc/redis/redis.conf`
- 哨兵配置: `/etc/redis/sentinel.conf`
- 数据目录: `/var/lib/redis`
- 日志目录: `/var/log/redis`
- 备份目录: `/backup/redis`
