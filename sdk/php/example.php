<?php
/**
 * WePay PHP SDK 使用示例
 */

require_once 'WePay.php';

// ============ MD5 签名示例 ============

$wepay = new WePay(
    'M515637',                          // 商户号
    'nca3twhvpqveixu2hdeutb6utpiet6k7', // 商户密钥
    'http://127.0.0.1:8088',            // API 基础 URL
    'MD5'                               // 签名方式
);

// 创建订单
$orderParams = [
    'out_trade_no' => 'order_' . time(),
    'pay_type' => 'wxpay',
    'amount' => '0.01',
    'subject' => 'PHP SDK 测试订单'
];

try {
    $result = $wepay->createOrder($orderParams);
    echo "MD5 签名下单结果:\n";
    echo json_encode($result, JSON_UNESCAPED_UNICODE | JSON_PRETTY_PRINT);
    echo "\n\n";
} catch (Exception $e) {
    echo "错误: " . $e->getMessage() . "\n";
}

// ============ RSA 签名示例 ============

$privateKeyPem = <<<'EOD'
-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQDM/CnqlQujQq7y
T5sWEq0KhbAimN8AgvO+J4tbnlQaOzZxRPDmKNEUKJcqgbILuaFCmXe6nSann27s
8WvUsnxXK5FGgNYmVOat/wacIkz5wk/KkJDvK3wuB/a7+jmfc/2dgxoOAc+NT34E
0LvvYaOhLHAl6Nyz6KXnSpdE2SXJKVibEF1O/debGo3bQz+OTJFJM8qqE6DiEiZb
VnUNbeYwq2Fmj/WXVUF5nnzXqVS+is9vtvH2hsRGBA+NhHAeA8WDZrHfh3BCqHaS
L++SaWici87+2a7LfRir+ojmnMs8aqGNIiKXmR+hkCdYp+InLf/pkNX17Cco6UdM
J2q1BZENAgMBAAECggEAG8bo5C+dw9oPrGiypo9R0Qz0JQALqf1Uy7X+maPvGB3h
fwBdV4b87AsjDuDD0HhvXH/A3HIasJi3dpaxasFj/Yj7Fu9y9X9IQhg+nE4+mZKl
7thft3UwTumH2wmpoMyeN6eyEmdW6Xp15G+no+TagEbuDIkNTTjPsHOoY208hFFe
p30Wu4JNA8hbk+aKl3mx8tL/iW1T0zLzi7vvXwBd/I7E/vRIZ98GVPQWbqJsy844
aa4rM9bqnR1nxGmsBC3KeY4sOI7o0Z/jFhjDQQCAFp9OWBS1BQPVQjvjXWv9ky4v
UofEVrFH1PNP5ec7M/Zv7GkK3BZuH1+8IFER3dNcSQKBgQDo2THUUPCJx4/aRSjD
ZClFjsnZJB7UvhEOUwur9vCTnARycEqJPzjJPKr8jMxdYn04DXmAQvQhWQobUA8S
9SDBZIAi20ZIMSmTe4YC2cc2YK3d+wJArigZMmV9BhES7D8Uces3gxMM2y/WDa3j
WXWkoAslfkmNQydfqmujcW9gNQKBgQDhXb+KVlMbP3489VIgnIqkAFHDGbtAr1uF
vYRbIaNz+mxqENzaMJ79X5h6VzDH7vsSQH051e4wvlIAHy2WlhCqXBhE3lRjI807
KwUBiJDA73VcOxfRdVYHHoPxObPpSMafaX1yIgwM7k+hpb9EBHNBfBlfomXhJ59m
TPiW7q+4eQKBgQCeLS1UdcdxUUe/lsuiMCB5SA6Gm6r2Cke7215Ka23yWEING4sG
wRPqYHQnK96IcadutHidUN5W6Q2ckD4tOqgNuB/zjdGoqPz9WyQmO5rArdxut11I
YwaKV1nqHHzsxd/0G48WHsyKJzvPxWsizlrEgpQP3EJK3BubOUH1vdFTIQKBgH8V
Ollr7FlFKI5/V9yD6bopY/G8pNcJC3cTM3ugMGfKIzB8ac2v9TeznGwAlsVngbT9
IKBofnSGHf9rlW2BGcy3Ogg7xyJQof5nd98xf08MuQVVXU0D+YryLjzs6QL3wulJ
ty+Q+3KfP9BLgtt8FvIqZLSFAyZADabGaLfTyMshAoGAFQSbx091g5+xX7GP5DMo
nv4BRVtXZ5d7nl2WaI1r5t7+mhFZyC5H7m1fe5A1kxfL1A38xs5iw8yBKDl8cDIT
eHvB50Lgh8MINsSh+c+InpX05WXh5jlKxcvbuFgGWxcz8QYoiOWIfcCRf6az+lfe
qZZfIrQB3NrrzdAZ5mZhOhw=
-----END PRIVATE KEY-----
EOD;

$wepayRsa = new WePay(
    'M515637',                          // 商户号
    'nca3twhvpqveixu2hdeutb6utpiet6k7', // 商户密钥（RSA 模式下不使用）
    'http://127.0.0.1:8088',            // API 基础 URL
    'RSA',                              // 签名方式
    $privateKeyPem                      // RSA 私钥
);

// 创建订单（RSA 签名）
$orderParams2 = [
    'out_trade_no' => 'order_rsa_' . time(),
    'pay_type' => 'wxpay',
    'amount' => '0.01',
    'subject' => 'PHP SDK RSA 签名测试订单'
];

try {
    $result = $wepayRsa->createOrder($orderParams2);
    echo "RSA 签名下单结果:\n";
    echo json_encode($result, JSON_UNESCAPED_UNICODE | JSON_PRETTY_PRINT);
    echo "\n\n";
} catch (Exception $e) {
    echo "错误: " . $e->getMessage() . "\n";
}

// ============ 查询订单 ============

try {
    $queryResult = $wepay->queryOrder('order_' . time());
    echo "查询订单结果:\n";
    echo json_encode($queryResult, JSON_UNESCAPED_UNICODE | JSON_PRETTY_PRINT);
    echo "\n\n";
} catch (Exception $e) {
    echo "错误: " . $e->getMessage() . "\n";
}

// ============ 异步通知验证 ============

$notifyData = [
    'trade_no' => 'W20260530152204174895',
    'out_trade_no' => 'order_123',
    'amount' => '0.01',
    'status' => '1'
];

$notifySign = $wepay->sign($notifyData);
$isValid = $wepay->verifyNotify($notifyData, $notifySign);

echo "异步通知验证结果: " . ($isValid ? "✓ 有效" : "✗ 无效") . "\n";
?>
