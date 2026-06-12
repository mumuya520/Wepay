# RocketMQ 集群配置指南

## 架构说明

### 集群模式：2m-2s-async（2主2从异步复制）
- **NameServer**: 3个节点（192.168.1.30:9876, 192.168.1.31:9876, 192.168.1.32:9876）
- **Broker Master1**: 192.168.1.33:10911
- **Broker Slave1**: 192.168.1.34:10911
- **Broker Master2**: 192.168.1.35:10911
- **Broker Slave2**: 192.168.1.36:10911

### 特点
- **高可用**: 主节点故障时从节点可读
- **高性能**: 异步复制，写入性能高
- **数据安全**: 主从复制，数据不丢失

---

## 一、NameServer 配置

### 1. 配置文件（所有 NameServer 节点）

#### namesrv.properties

```bash
# 创建配置文件
sudo mkdir -p /opt/rocketmq/conf
sudo vim /opt/rocketmq/conf/namesrv.properties
```

```properties
# NameServer 监听端口
listenPort=9876

# 日志目录
rocketmqHome=/opt/rocketmq

# 删除未使用的 Topic
deleteWhen=04
fileReservedTime=48

# KV 配置文件路径
kvConfigPath=/opt/rocketmq/store/kvConfig.json
```

### 2. 启动 NameServer

#### 启动脚本（所有节点）

```bash
#!/bin/bash
# start-namesrv.sh

export JAVA_HOME=/usr/lib/jvm/java-11-openjdk-amd64
export ROCKETMQ_HOME=/opt/rocketmq

# JVM 参数
export JAVA_OPT="-server -Xms2g -Xmx2g -Xmn1g -XX:MetaspaceSize=128m -XX:MaxMetaspaceSize=320m"

# 启动 NameServer
nohup sh ${ROCKETMQ_HOME}/bin/mqnamesrv \
    -c ${ROCKETMQ_HOME}/conf/namesrv.properties \
    > ${ROCKETMQ_HOME}/logs/namesrv.log 2>&1 &

echo "NameServer started"
```

#### 在所有 NameServer 节点执行

```bash
# 节点1（192.168.1.30）
sudo bash /opt/rocketmq/bin/start-namesrv.sh

# 节点2（192.168.1.31）
sudo bash /opt/rocketmq/bin/start-namesrv.sh

# 节点3（192.168.1.32）
sudo bash /opt/rocketmq/bin/start-namesrv.sh

# 验证
jps | grep NamesrvStartup
netstat -tuln | grep 9876
```

---

## 二、Broker 配置

### 1. Broker Master1 配置（192.168.1.33）

#### broker-a.properties

```properties
# Broker 名称（同一组主从使用相同名称）
brokerClusterName=WepayRocketMQCluster
brokerName=broker-a
brokerId=0

# 监听端口
listenPort=10911

# NameServer 地址
namesrvAddr=192.168.1.30:9876;192.168.1.31:9876;192.168.1.32:9876

# Broker IP（外网访问时需要配置）
brokerIP1=192.168.1.33

# 角色（ASYNC_MASTER, SYNC_MASTER, SLAVE）
brokerRole=ASYNC_MASTER

# 刷盘方式（ASYNC_FLUSH, SYNC_FLUSH）
flushDiskType=ASYNC_FLUSH

# 存储路径
storePathRootDir=/opt/rocketmq/store/broker-a
storePathCommitLog=/opt/rocketmq/store/broker-a/commitlog
storePathConsumeQueue=/opt/rocketmq/store/broker-a/consumequeue
storePathIndex=/opt/rocketmq/store/broker-a/index

# 删除文件时间点（凌晨4点）
deleteWhen=04

# 文件保留时间（48小时）
fileReservedTime=48

# CommitLog 文件大小（1GB）
mapedFileSizeCommitLog=1073741824

# ConsumeQueue 文件大小（300KB）
mapedFileSizeConsumeQueue=300000

# 是否允许自动创建 Topic
autoCreateTopicEnable=false

# 是否允许自动创建订阅组
autoCreateSubscriptionGroup=true

# 消息最大大小（4MB）
maxMessageSize=4194304

# 发送消息线程池数量
sendMessageThreadPoolNums=16

# 拉取消息线程池数量
pullMessageThreadPoolNums=16

# 查询消息线程池数量
queryMessageThreadPoolNums=8

# 管理线程池数量
adminBrokerThreadPoolNums=16

# 客户端管理线程池数量
clientManageThreadPoolNums=32

# 消费者管理线程池数量
consumerManageThreadPoolNums=32

# 心跳线程池数量
heartbeatThreadPoolNums=8

# 事务消息检查间隔（60秒）
transactionCheckInterval=60000

# 事务消息检查最大次数
transactionCheckMax=15

# 是否开启消息轨迹
traceTopicEnable=true

# 是否开启 ACL
aclEnable=false

# 是否开启消息过滤
enablePropertyFilter=true
```

### 2. Broker Slave1 配置（192.168.1.34）

#### broker-a-s.properties

```properties
# Broker 名称（与 Master 相同）
brokerClusterName=WepayRocketMQCluster
brokerName=broker-a
brokerId=1

# 监听端口
listenPort=10911

# NameServer 地址
namesrvAddr=192.168.1.30:9876;192.168.1.31:9876;192.168.1.32:9876

# Broker IP
brokerIP1=192.168.1.34

# 角色（SLAVE）
brokerRole=SLAVE

# 刷盘方式
flushDiskType=ASYNC_FLUSH

# 存储路径
storePathRootDir=/opt/rocketmq/store/broker-a-s
storePathCommitLog=/opt/rocketmq/store/broker-a-s/commitlog
storePathConsumeQueue=/opt/rocketmq/store/broker-a-s/consumequeue
storePathIndex=/opt/rocketmq/store/broker-a-s/index

# 删除文件时间点
deleteWhen=04

# 文件保留时间
fileReservedTime=48

# CommitLog 文件大小
mapedFileSizeCommitLog=1073741824

# ConsumeQueue 文件大小
mapedFileSizeConsumeQueue=300000

# 不允许自动创建 Topic
autoCreateTopicEnable=false

# 允许自动创建订阅组
autoCreateSubscriptionGroup=true

# 消息最大大小
maxMessageSize=4194304

# 线程池配置（同 Master）
sendMessageThreadPoolNums=16
pullMessageThreadPoolNums=16
queryMessageThreadPoolNums=8
adminBrokerThreadPoolNums=16
clientManageThreadPoolNums=32
consumerManageThreadPoolNums=32
heartbeatThreadPoolNums=8
```

### 3. Broker Master2 配置（192.168.1.35）

#### broker-b.properties

```properties
# Broker 名称
brokerClusterName=WepayRocketMQCluster
brokerName=broker-b
brokerId=0

# 监听端口
listenPort=10911

# NameServer 地址
namesrvAddr=192.168.1.30:9876;192.168.1.31:9876;192.168.1.32:9876

# Broker IP
brokerIP1=192.168.1.35

# 角色
brokerRole=ASYNC_MASTER

# 刷盘方式
flushDiskType=ASYNC_FLUSH

# 存储路径
storePathRootDir=/opt/rocketmq/store/broker-b
storePathCommitLog=/opt/rocketmq/store/broker-b/commitlog
storePathConsumeQueue=/opt/rocketmq/store/broker-b/consumequeue
storePathIndex=/opt/rocketmq/store/broker-b/index

# 其他配置同 broker-a
deleteWhen=04
fileReservedTime=48
mapedFileSizeCommitLog=1073741824
mapedFileSizeConsumeQueue=300000
autoCreateTopicEnable=false
autoCreateSubscriptionGroup=true
maxMessageSize=4194304
sendMessageThreadPoolNums=16
pullMessageThreadPoolNums=16
queryMessageThreadPoolNums=8
adminBrokerThreadPoolNums=16
clientManageThreadPoolNums=32
consumerManageThreadPoolNums=32
heartbeatThreadPoolNums=8
```

### 4. Broker Slave2 配置（192.168.1.36）

#### broker-b-s.properties

```properties
# Broker 名称（与 Master 相同）
brokerClusterName=WepayRocketMQCluster
brokerName=broker-b
brokerId=1

# 监听端口
listenPort=10911

# NameServer 地址
namesrvAddr=192.168.1.30:9876;192.168.1.31:9876;192.168.1.32:9876

# Broker IP
brokerIP1=192.168.1.36

# 角色
brokerRole=SLAVE

# 刷盘方式
flushDiskType=ASYNC_FLUSH

# 存储路径
storePathRootDir=/opt/rocketmq/store/broker-b-s
storePathCommitLog=/opt/rocketmq/store/broker-b-s/commitlog
storePathConsumeQueue=/opt/rocketmq/store/broker-b-s/consumequeue
storePathIndex=/opt/rocketmq/store/broker-b-s/index

# 其他配置同 broker-a-s
deleteWhen=04
fileReservedTime=48
mapedFileSizeCommitLog=1073741824
mapedFileSizeConsumeQueue=300000
autoCreateTopicEnable=false
autoCreateSubscriptionGroup=true
maxMessageSize=4194304
sendMessageThreadPoolNums=16
pullMessageThreadPoolNums=16
queryMessageThreadPoolNums=8
adminBrokerThreadPoolNums=16
clientManageThreadPoolNums=32
consumerManageThreadPoolNums=32
heartbeatThreadPoolNums=8
```

---

## 三、启动 Broker

### 1. 启动脚本

#### start-broker.sh

```bash
#!/bin/bash
# start-broker.sh

export JAVA_HOME=/usr/lib/jvm/java-11-openjdk-amd64
export ROCKETMQ_HOME=/opt/rocketmq

# JVM 参数
export JAVA_OPT="-server -Xms4g -Xmx4g -Xmn2g -XX:MetaspaceSize=128m -XX:MaxMetaspaceSize=320m"

# 配置文件路径（根据节点不同修改）
BROKER_CONF=$1

if [ -z "$BROKER_CONF" ]; then
    echo "Usage: $0 <broker-config-file>"
    exit 1
fi

# 启动 Broker
nohup sh ${ROCKETMQ_HOME}/bin/mqbroker \
    -c ${ROCKETMQ_HOME}/conf/${BROKER_CONF} \
    > ${ROCKETMQ_HOME}/logs/broker.log 2>&1 &

echo "Broker started with config: ${BROKER_CONF}"
```

### 2. 启动所有 Broker

```bash
# Broker Master1（192.168.1.33）
sudo bash /opt/rocketmq/bin/start-broker.sh broker-a.properties

# Broker Slave1（192.168.1.34）
sudo bash /opt/rocketmq/bin/start-broker.sh broker-a-s.properties

# Broker Master2（192.168.1.35）
sudo bash /opt/rocketmq/bin/start-broker.sh broker-b.properties

# Broker Slave2（192.168.1.36）
sudo bash /opt/rocketmq/bin/start-broker.sh broker-b-s.properties

# 验证
jps | grep BrokerStartup
netstat -tuln | grep 10911
```

---

## 四、创建 Topic

### 1. 使用 mqadmin 创建 Topic

```bash
# 设置 NameServer 地址
export NAMESRV_ADDR=192.168.1.30:9876;192.168.1.31:9876;192.168.1.32:9876

# 创建 Topic
sh /opt/rocketmq/bin/mqadmin updateTopic \
    -n ${NAMESRV_ADDR} \
    -c WepayRocketMQCluster \
    -t order-push-topic \
    -r 8 \
    -w 8

sh /opt/rocketmq/bin/mqadmin updateTopic \
    -n ${NAMESRV_ADDR} \
    -c WepayRocketMQCluster \
    -t ocr-task-topic \
    -r 8 \
    -w 8

sh /opt/rocketmq/bin/mqadmin updateTopic \
    -n ${NAMESRV_ADDR} \
    -c WepayRocketMQCluster \
    -t image-compress-topic \
    -r 8 \
    -w 8

sh /opt/rocketmq/bin/mqadmin updateTopic \
    -n ${NAMESRV_ADDR} \
    -c WepayRocketMQCluster \
    -t email-notify-topic \
    -r 8 \
    -w 8

sh /opt/rocketmq/bin/mqadmin updateTopic \
    -n ${NAMESRV_ADDR} \
    -c WepayRocketMQCluster \
    -t alert-notify-topic \
    -r 8 \
    -w 8

sh /opt/rocketmq/bin/mqadmin updateTopic \
    -n ${NAMESRV_ADDR} \
    -c WepayRocketMQCluster \
    -t order-timeout-topic \
    -r 8 \
    -w 8

sh /opt/rocketmq/bin/mqadmin updateTopic \
    -n ${NAMESRV_ADDR} \
    -c WepayRocketMQCluster \
    -t device-status-topic \
    -r 8 \
    -w 8

# 查看 Topic 列表
sh /opt/rocketmq/bin/mqadmin topicList -n ${NAMESRV_ADDR}

# 查看 Topic 详情
sh /opt/rocketmq/bin/mqadmin topicStatus -n ${NAMESRV_ADDR} -t order-push-topic
```

---

## 五、应用层配置

### 1. C++ 客户端配置

#### RocketMQManager.cpp

```cpp
#include <rocketmq/DefaultMQProducer.h>
#include <rocketmq/DefaultMQPushConsumer.h>

using namespace rocketmq;

// 初始化生产者
void RocketMQProducerManager::init(const std::string& namesrvAddr) {
    // 创建生产者
    producer_ = std::make_shared<DefaultMQProducer>("WepayProducerGroup");
    
    // 设置 NameServer 地址
    producer_->setNamesrvAddr(namesrvAddr);
    
    // 设置实例名称
    producer_->setInstanceName("WepayProducer");
    
    // 设置发送超时时间（3秒）
    producer_->setSendMsgTimeout(3000);
    
    // 设置重试次数
    producer_->setRetryTimes(3);
    
    // 设置最大消息大小（4MB）
    producer_->setMaxMessageSize(4 * 1024 * 1024);
    
    // 设置压缩阈值（4KB）
    producer_->setCompressMsgBodyOverHowmuch(4096);
    
    // 启动生产者
    producer_->start();
    
    LOG_INFO << "RocketMQ Producer started, NameServer: " << namesrvAddr;
}

// 初始化消费者
void OrderPushConsumer::start(const std::string& namesrvAddr) {
    // 创建消费者
    consumer_ = std::make_shared<DefaultMQPushConsumer>("WepayOrderPushConsumerGroup");
    
    // 设置 NameServer 地址
    consumer_->setNamesrvAddr(namesrvAddr);
    
    // 设置实例名称
    consumer_->setInstanceName("WepayOrderPushConsumer");
    
    // 订阅 Topic
    consumer_->subscribe("order-push-topic", "*");
    
    // 设置消费模式（集群消费）
    consumer_->setMessageModel(CLUSTERING);
    
    // 设置消费线程数
    consumer_->setConsumeThreadCount(20);
    
    // 设置最大重试次数
    consumer_->setMaxReconsumeTimes(3);
    
    // 注册消息监听器
    consumer_->registerMessageListener(this);
    
    // 启动消费者
    consumer_->start();
    
    LOG_INFO << "RocketMQ Consumer started, Topic: order-push-topic";
}
```

### 2. WepayV3Config.yaml

```yaml
rocketmq:
  # NameServer 地址（多个用分号分隔）
  namesrvAddr: "192.168.1.30:9876;192.168.1.31:9876;192.168.1.32:9876"
  
  # 生产者配置
  producer:
    groupName: "WepayProducerGroup"
    sendTimeout: 3000
    retryTimes: 3
    maxMessageSize: 4194304
    
  # 消费者配置
  consumer:
    groupName: "WepayConsumerGroup"
    consumeThreadCount: 20
    maxReconsumeTimes: 3
    
  # Topic 配置
  topics:
    - name: "order-push-topic"
      queueNum: 8
    - name: "ocr-task-topic"
      queueNum: 8
    - name: "email-notify-topic"
      queueNum: 8
    - name: "alert-notify-topic"
      queueNum: 8
    - name: "order-timeout-topic"
      queueNum: 8
    - name: "device-status-topic"
      queueNum: 8
```

---

## 六、监控与管理

### 1. 查看集群状态

```bash
# 查看集群信息
sh /opt/rocketmq/bin/mqadmin clusterList -n ${NAMESRV_ADDR}

# 查看 Broker 状态
sh /opt/rocketmq/bin/mqadmin brokerStatus -n ${NAMESRV_ADDR} -b 192.168.1.33:10911

# 查看消费者组
sh /opt/rocketmq/bin/mqadmin consumerProgress -n ${NAMESRV_ADDR}

# 查看消息堆积
sh /opt/rocketmq/bin/mqadmin consumerProgress -n ${NAMESRV_ADDR} -g WepayOrderPushConsumerGroup
```

### 2. RocketMQ Console（Web 管理界面）

```bash
# 下载 RocketMQ Console
git clone https://github.com/apache/rocketmq-dashboard.git
cd rocketmq-dashboard

# 修改配置
vim src/main/resources/application.properties

# 配置 NameServer 地址
rocketmq.config.namesrvAddr=192.168.1.30:9876;192.168.1.31:9876;192.168.1.32:9876

# 编译运行
mvn clean package -Dmaven.test.skip=true
java -jar target/rocketmq-dashboard-1.0.0.jar

# 访问 http://localhost:8080
```

---

## 七、故障切换

### 1. Master 故障

当 Master 故障时：
- **写入**: 失败，需要手动切换到另一个 Master
- **读取**: 自动切换到 Slave

### 2. 手动切换 Master

```bash
# 停止故障的 Master
sh /opt/rocketmq/bin/mqshutdown broker

# 修改 Slave 配置为 Master
vim /opt/rocketmq/conf/broker-a-s.properties

# 修改 brokerId 和 brokerRole
brokerId=0
brokerRole=ASYNC_MASTER

# 重启 Broker
sh /opt/rocketmq/bin/start-broker.sh broker-a-s.properties
```

### 3. 自动故障切换（DLedger 模式）

推荐使用 RocketMQ 4.5+ 的 DLedger 模式实现自动故障切换。

---

## 八、性能优化

### 1. 操作系统优化

```bash
# 增加文件描述符限制
ulimit -n 655350

# 禁用 swap
swapoff -a

# 调整内核参数
sysctl -w vm.overcommit_memory=1
sysctl -w vm.max_map_count=655360
```

### 2. JVM 优化

```bash
# 增加堆内存
export JAVA_OPT="-server -Xms8g -Xmx8g -Xmn4g"

# 使用 G1 垃圾回收器
export JAVA_OPT="${JAVA_OPT} -XX:+UseG1GC -XX:MaxGCPauseMillis=50"

# GC 日志
export JAVA_OPT="${JAVA_OPT} -Xlog:gc*:file=/opt/rocketmq/logs/gc.log:time,tags:filecount=5,filesize=30M"
```

### 3. Broker 优化

```properties
# 增加发送线程数
sendMessageThreadPoolNums=32

# 增加拉取线程数
pullMessageThreadPoolNums=32

# 开启消息过滤
enablePropertyFilter=true

# 开启消息压缩
compressedRegister=true
```

---

## 九、备份与恢复

### 1. 备份 CommitLog

```bash
#!/bin/bash
# backup-rocketmq.sh

BACKUP_DIR="/backup/rocketmq"
DATE=$(date +%Y%m%d)

# 备份 broker-a
rsync -avz /opt/rocketmq/store/broker-a/commitlog/ ${BACKUP_DIR}/${DATE}/broker-a/

# 备份 broker-b
rsync -avz /opt/rocketmq/store/broker-b/commitlog/ ${BACKUP_DIR}/${DATE}/broker-b/

# 保留最近7天的备份
find ${BACKUP_DIR} -type d -mtime +7 -exec rm -rf {} \;
```

### 2. 恢复数据

```bash
# 停止 Broker
sh /opt/rocketmq/bin/mqshutdown broker

# 恢复 CommitLog
rsync -avz ${BACKUP_DIR}/20240123/broker-a/ /opt/rocketmq/store/broker-a/commitlog/

# 启动 Broker
sh /opt/rocketmq/bin/start-broker.sh broker-a.properties
```

---

## 十、安全配置

### 1. 启用 ACL

```properties
# broker.properties
aclEnable=true
```

#### plain_acl.yml

```yaml
accounts:
  - accessKey: wepay_producer
    secretKey: wepay_producer_secret_123
    whiteRemoteAddress: 192.168.1.*
    admin: false
    defaultTopicPerm: PUB
    defaultGroupPerm: DENY
    topicPerms:
      - order-push-topic=PUB
      - ocr-task-topic=PUB
      - email-notify-topic=PUB
      
  - accessKey: wepay_consumer
    secretKey: wepay_consumer_secret_123
    whiteRemoteAddress: 192.168.1.*
    admin: false
    defaultTopicPerm: SUB
    defaultGroupPerm: SUB
    topicPerms:
      - order-push-topic=SUB
      - ocr-task-topic=SUB
```

### 2. 应用层使用 ACL

```cpp
// 设置 AccessKey 和 SecretKey
producer_->setSessionCredentials("wepay_producer", "wepay_producer_secret_123", "");
consumer_->setSessionCredentials("wepay_consumer", "wepay_consumer_secret_123", "");
```

---

## 配置文件位置

- NameServer 配置: `/opt/rocketmq/conf/namesrv.properties`
- Broker 配置: `/opt/rocketmq/conf/broker-*.properties`
- ACL 配置: `/opt/rocketmq/conf/plain_acl.yml`
- 数据目录: `/opt/rocketmq/store`
- 日志目录: `/opt/rocketmq/logs`
- 备份目录: `/backup/rocketmq`
