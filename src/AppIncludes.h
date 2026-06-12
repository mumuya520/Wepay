// WePay-Cpp — 控制器/插件/过滤器统一 include 头
// 使用方法: main.cc 里 `#include "AppIncludes.h"` 即可，
// 新增模块只需在此处添加一行，保持 main.cc 简洁
#pragma once // 防止头文件重复包含

// ══════════════════════════════════════════════════════════════
// 通用服务 / 过滤器
// ══════════════════════════════════════════════════════════════
#include "common/PayDb.h" // 数据库连接和 SQL 执行
#include "common/DatabaseInit.h" // 数据库初始化和迁移
#include "common/SimpleJwt.h" // JWT 令牌生成和验证
#include "common/DeviceKeyUtils.h" // 设备密钥工具（EdDSA/RSA）
#include "common/DeviceChallengeCache.h" // 设备挑战缓存
#include "common/DeviceKeyEnforcement.h" // 设备密钥强制执行
#include "common/DeviceKeyStatusCtrl.h" // 设备密钥状态控制
#include "common/IpVerifyUtils.h" // IP 验证工具
#include "common/TokenService.h" // 令牌服务
#include "common/NotifyTaskService.h" // 异步通知任务队列
#include "common/PrintTaskService.h" // 打印任务队列
#include "common/OplogService.h" // 操作日志服务
#include "common/SyncHttp.h" // 同步 HTTP 客户端
#include "common/HttpStatus.h" // HTTP 状态码全集 + 扩展 RESP_xxx 宏（兼容原有宏）
#include "common/CacheService.h" // 缓存服务（Redis）
#include "common/MqService.h" // 消息队列服务（RabbitMQ/RocketMQ）
#include "common/SwaggerCtrl.h" // Swagger API 文档
#include "common/AgPayProxy.h" // /plus/* 反向代理到 AgPay Plus
#include "common/DujiaoProxy.h" // /dujiao/* 嵌入 dujiao.dll + SSO 反代
#include "common/MpayProxy.h" // /mpay/* 反向代理到 MPay v2 Webman (PHP)
#include "common/PhpPayProxy.h" // /phppay/* 反向代理到 php-pay 项目 (PHP)
#include "common/SamWafProxy.h" // SamWaf WAF 子进程托管 + /waf/ 反代
#include "common/NginxProxy.h" // [已弃用] Nginx 子进程托管，Linux 不可用，由下面两个模块取代
#include "common/NginxLikeFeatures.h" // 进程内 nginx 功能: limit_conn/ip_acl/proxy_pass/access_log
#include "common/HttpsRedirect.h" // 可选 HTTP→HTTPS 强制跳转 (HTTPS 监听走 Drogon 原生 listeners)
#include "ai/KoboldCppService.h" // KoboldCpp 本地 LLM 封装
#include "ai/KoboldCppManager.h" // KoboldCpp 子进程托管 + 自动重启 (/admin/api/ai/kobold/*)
#include "ai/AiSecurityGateway.h" // AI 智能防护网关 (全流量分析 + 威胁日志)
#include "ai/AiChatCtrl.h" // AI 智能助手 (GET /ai + /admin/api/ai/*)
#include "common/VaultClient.h" // HashiCorp Vault KV-v2 HTTP 客户端（敏感信息存储）
#include "common/VaultProxy.h" // Vault 子进程托管 + 自动解封 (/admin/api/vault/*)
#include "common/SystemHealthCtrl.h" // GET /health GET /admin/api/system/health
#include "common/BillService.h" // 账单生成
#include "common/LoginAttemptService.h" // 防爆破
#include "common/NotifyChannels.h" // SMS/Email/OSS
#include "common/WsBus.h" // WebSocket 消息总线
#include "common/Utilities.h" // 二维码/验证码/IP/UA/国密/金额/雪花ID
#include "common/CaptchaCtrl.h" // 图形验证码 API
#include "common/CronService.h" // 定时任务: 过期关闭/自动结算/统计/清理
#include "common/NacosService.h" // Nacos 服务注册/发现/配置中心
#include "common/MsgNoticeService.h" // 消息通知: 邮件/webhook/短信
#include "common/RiskControlService.h" // 风控: IP限频/域名白名单/黑名单/限额
#include "common/GopayService.h" // GoPay 支付网关服务
#include "common/UpaySharedService.h" // UPay shared 动态库桥接服务
#include "common/UpayProxy.h" // /upay/* 静态托管 UPay 前端
#include "common/JsonLogger.h" // JSON 格式日志(本地时间) + 彩色控制台
#include "common/Banner.h" // 佛祖启动横幅
#include "common/LicenseClient.h" // 远程授权客户端

#include "filters/AdminAuthFilter.h" // 管理员 JWT 认证过滤器
#include "filters/MerchantAuthFilter.h" // 商户 JWT 认证过滤器
#include "filters/AgentAuthFilter.h" // 代理 JWT 认证过滤器
#include "filters/RateLimitFilter.h" // 速率限制过滤器
#include "filters/SecurityHeaderFilter.h" // 安全响应头过滤器

// ══════════════════════════════════════════════════════════════
// 通道插件 (注册到 ChannelPluginRegistry)
// ══════════════════════════════════════════════════════════════
#include "channel/ChannelPlugin.h" // 通道插件基类
#include "channel/WxpayNativePlugin.h" // 微信原生扫码支付
#include "channel/WxpayExtPlugin.h" // 付款码/JSAPI/H5/APP
#include "channel/AlipayPlugin.h" // 支付宝基础支付
#include "channel/AlipayExtPlugin.h" // 付款码/JSAPI/WAP/PAGE
#include "channel/AlipaySandboxPlugin.h" // 支付宝沙箱
#include "channel/EpayUpstreamPlugin.h" // 易支付上游通道
#include "channel/MonitorPlugin.h" // 监听端通道
#include "channel/VmqPlugin.h" // VMQ 支付通道
#include "channel/WepayPlugin.h" // WePay 原生支付
#include "channel/WepayV3Plugin.h" // WePay V3 支付
#include "channel/XpayPlugin.h" // xpay-go 原生支付宝/微信通道
#include "channel/CodePayPlugin.h" // 码支付通道
#include "channel/AlipayMonitorPlugin.h" // 支付宝免签 (独立订单池)
#include "channel/WxMonitorPlugin.h" // 微信免签
#include "channel/QqMonitorPlugin.h" // QQ 免签
#include "channel/UnionPayPlugin.h" // 银联支付
#include "channel/AllinPayPlugin.h" // 通联支付
#include "channel/DgPayPlugin.h" // 斗拱(汇付)
#include "channel/LklPayPlugin.h" // 拉卡拉
#include "channel/JeepayPlugin.h" // Jeepay 聚合支付
#include "channel/AgpayPlusPlugin.h" // AgPay Plus 聚合支付
#include "channel/AdapayPlugin.h" // AdaPay 聚合支付
#include "channel/AlipayCodePlugin.h" // 支付宝免签约码支付
#include "channel/AlipayDPlugin.h" // 支付宝官方支付直付通版
#include "channel/AlipayGPlugin.h" // 支付宝国际版(Antom)
#include "channel/AlipayHkPlugin.h" // AlipayHK
#include "channel/AlipayRpPlugin.h" // 支付宝现金红包
#include "channel/AlipaySlPlugin.h" // 支付宝官方支付服务商版
#include "channel/ChinaUmsPlugin.h" // 银联商务
#include "channel/DinPayPlugin.h" // 智付
#include "channel/DuolabaoPlugin.h" // 哆啦宝支付
#include "channel/EasypayPlugin.h" // 易生易企通
#include "channel/EpayPlugin.h" // 彩虹易支付
#include "channel/EpaynPlugin.h" // 彩虹易支付 V2
#include "channel/FubeiPlugin.h" // 付呗聚合支付
#include "channel/Fuiou2Plugin.h" // 富友支付(合作方)
#include "channel/HaipayPlugin.h" // 海科聚合支付
#include "channel/HeepayPlugin.h" // 汇付宝
#include "channel/HlpayPlugin.h" // 汇联支付
#include "channel/HnaPayPlugin.h" // 新生支付
#include "channel/HuifuPlugin.h" // 汇付斗拱平台
#include "channel/HuolianPlugin.h" // 火脸支付
#include "channel/JdpayPlugin.h" // 京东支付
#include "channel/JlpayPlugin.h" // 嘉联支付
#include "channel/KayixinPlugin.h" // 卡易信(钱多多分账)
#include "channel/KuaiqianPlugin.h" // 快钱支付
#include "channel/LakalaPlugin.h" // 拉卡拉
#include "channel/LeshuaPlugin.h" // 乐刷聚合支付
#include "channel/LtzfPlugin.h" // 蓝兔支付
#include "channel/LzyzfPlugin.h" // 浪子易支付
#include "channel/PasspayPlugin.h" // 精秀支付
#include "channel/PaypalPlugin.h" // PayPal
#include "channel/QqpayPlugin.h" // QQ 钱包官方支付
#include "channel/SandpayPlugin.h" // 杉德支付
#include "channel/ShengpayPlugin.h" // 盛付通
#include "channel/StripePlugin.h" // Stripe
#include "channel/SuixingpayPlugin.h" // 随行付
#include "channel/SwiftpassPlugin.h" // 威富通 RSA
#include "channel/Swiftpass2Plugin.h" // 威富通 MD5
#include "channel/UmfpayPlugin.h" // 联动优势
#include "channel/UnionpayGatewayPlugin.h" // 银联前置(威富通协议)
#include "channel/UsdtproPlugin.h" // USDTPRO V2
#include "channel/BepusdtPlugin.h" // BEpusdt 多链加密货币支付
#include "common/BepusdtProxy.h" // /crypto/* 嵌入 bepusdt.dll 反代
#include "channel/XorpayPlugin.h" // XorPay
#include "channel/XsyPlugin.h" // 新生易
#include "channel/XunhupayPlugin.h" // 虎皮椒支付
#include "channel/YeepayPlugin.h" // 易宝支付
#include "channel/YinyingtongPlugin.h" // 银盈通支付
#include "channel/YsepayPlugin.h" // 银盛支付
#include "channel/YseqtPlugin.h" // 银盛 e 企通
#include "channel/ZhangyishouPlugin.h" // 掌易收聚合支付
#include "channel/GenericPlugins.h" // 9 家中小三方支付插件
#include "channel/ReceiptPlugin.h" // 支付宝网页流水监听收款插件
#include "channel/SmsReceiptPlugin.h" // 手动确认免签收款(用户提交订单号+邮件审核)
#include "channel/AlipayEmailPlugin.h" // 邮件通知免签(IMAP 轮询支付宝收款通知)
#include "channel/PluginMarketCtrl.h" // 插件市场

// ══════════════════════════════════════════════════════════════
// 控制器 (Drogon 自动注册)
// ══════════════════════════════════════════════════════════════

// 旧版支付(V免签 / 码支付兼容)
#include "pay/PayOrderCtrl.h" // 订单管理控制器
#include "pay/MonitorCtrl.h" // 监听端控制器
#include "pay/WepayMonitorCtrl.h" // WePay V2 原生协议 (HMAC-SHA256)

/*#include "pay/WepayV3MonitorCtrl.h"            //  WePay V3 sorted-qs + OCR + 多商户
#include "pay/WepayV3WsCtrl.h"                 //  WePay V3 WebSocket 设备推单
#include "pay/WepayV3ManualCallbackCtrl.h"
*/
#ifdef WEPAY_HAS_V3 // 如果启用 WePay V3 插件
#include "channel/v3/WepayV3Plugin.h" // WePay V3 原生监控插件（redis++, 可选 RocketMQ）
#include "channel/v3/AdminV3Ctrl.h" // V3 管理后台 API
#endif
#include "pay/EpayCtrl.h" // 易支付控制器
#include "pay/PayPageCtrl.h" // 支付页面控制器

// /admin/api/* 管理员后台
#include "admin/AdminLoginCtrl.h" // 管理员登录
#include "admin/AdminDeviceKeyCtrl.h" // 设备密钥管理
#include "admin/OrderMgrCtrl.h" // 订单管理
#include "admin/QrcodeMgrCtrl.h" // 二维码管理
#include "admin/MonitorApkCtrl.h" // 监听端 APK 上传/下载
#include "admin/FileUploadCtrl.h" // 通用图片上传
#include "admin/XpayProxyCtrl.h" // xpay-go 反向代理 (/xpay/**)
#include "admin/ConfigMgrCtrl.h" // 配置管理
#include "admin/NotifyMgrCtrl.h" // 通知管理
#include "admin/AdminMerchantCtrl.h" // 商户管理
#include "admin/AdminChannelCtrl.h" // 通道管理
#include "admin/AdminSettleCtrl.h" // 结算管理
#include "admin/SysUserCtrl.h" // 系统用户管理
#include "admin/RoleCtrl.h" // 角色管理
#include "admin/AgentCtrl.h" // 代理管理
#include "admin/DivisionCtrl.h" // 分账管理
#include "admin/TransferCtrl.h" // 转账管理
#include "admin/IsvCtrl.h" // 服务商管理
#include "admin/MchAppCtrl.h" // 商户应用管理
#include "admin/MchStoreCtrl.h" // 商户门店管理
#include "admin/CustomFormCtrl.h" // 自定义表单 (form-create)
#include "admin/PayConfigCtrl.h" // 接口/费率/OAuth2 配置
#include "admin/AdminUtilsCtrl.h" // 图表/账单/公告
#include "admin/MetadataCtrl.h" // 支付方式/菜单/聚合二维码/商户扩展配置
#include "admin/AdminExtraCtrl.h" // 退款/分账组/QR模板/团队/日志
#include "admin/AdminSourceCompatCtrl.h" // 域名/工单/CDK/VIP/充值/统计
#include "admin/PluginCtrl.h" // 插件热加载管理
#include "admin/OpsCtrl.h" // 运维监控面板
#include "admin/PrintCtrl.h" // 打印控制器 (ESC/POS + 云打印)

// /merchant/api/* 商户后台
#include "merchant/MerchantLoginCtrl.h" // 商户登录
#include "merchant/MerchantDeviceKeyCtrl.h" // 商户设备密钥管理
#include "merchant/MerchantOrderCtrl.h" // 商户订单管理
#include "merchant/MerchantSettleCtrl.h" // 商户结算管理
#include "merchant/MerchantRefundCtrl.h" // 商户自助退款
#include "merchant/MerchantTransferCtrl.h" // 商户自助转账
#include "merchant/MchUserCtrl.h" // 商户子账号管理
#include "merchant/MerchantChannelCtrl.h" // 商户支付通道管理（获取/绑定/解绑/费率）
#include "merchant/MerchantExtraCtrl.h" // 支付测试/通道查看/扩展配置
#include "merchant/MerchantSourceCompatCtrl.h" // 域名/工单/CDK/实名/谷歌/返利
#include "merchant/MerchantFileUploadCtrl.h" // 商户文件上传（收款码）

// /agent/api/* 代理后台
#include "agent/AgentSelfCtrl.h" // 代理自服务
#include "agent/AgentExtraCtrl.h" // 代理子账号+下辖商户接口配置查看

// /gateway/* 统一支付网关
#include "gateway/GatewayCtrl.h" // 网关主控制器
#include "gateway/SimpleOrderCtrl.h" // 简单下单接口 (API Key 认证)
#include "gateway/SecureGatewayCtrl.h" // RSA 签名网关 (安全版)
#include "gateway/StandardPayCtrl.h" // 标准支付接口 (兼容主流聚合支付)
#include "gateway/NotifyCallbackCtrl.h" // 异步通知回调
#include "gateway/TestNotifyCtrl.h" // 内置测试回调接收端
#include "gateway/ChannelUserIdCtrl.h" // OAuth2 获取 openid/buyer_id
#include "gateway/WsOrderCtrl.h" // WebSocket 订单实时通知
#include "gateway/WsLiveCtrl.h" // WebSocket 实时事件 (监控端扫码)
#include "gateway/WeWorkCtrl.h" // 企业微信客服支付

// /api/alipay/* 支付宝 Scheme 生成（已集成到网关）
#include "alipay/AlipayCtrl.h" // 支付宝控制器

// /notify/* 第三方异步回调
#include "notify/NotifyReceiveCtrl.h" // 异步回调接收
#include "notify/CefBillCtrl.h" // /notify/cef/{bill,cookie} 接收 webpay (CEF) 推送

// /device/* 自建设备上报
#include "device/DeviceCtrl.h" // 设备上报控制器

// 网页流水监听 (receipt_watcher)
#include "common/ReceiptWatcherService.h" // 网页流水监听服务

// 公开 API (含运行时配置)
#include "common/AppConfigCtrl.h" // 应用配置控制器


/**/