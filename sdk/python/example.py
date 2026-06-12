#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
WePay Python SDK 使用示例
"""

import time
import json
from wepay import WePay

# ============ MD5 签名示例 ============

print("=" * 80)
print("WePay Python SDK 示例")
print("=" * 80)

# 初始化 SDK（MD5 签名）
wepay = WePay(
    mch_id='M515637',
    mch_key='nca3twhvpqveixu2hdeutb6utpiet6k7',
    base_url='http://127.0.0.1:8088',
    sign_type='MD5'
)

print("\n[1] MD5 签名 - 统一下单")
print("-" * 80)

order_params = {
    'out_trade_no': f'order_py_{int(time.time() * 1000)}',
    'pay_type': 'wxpay',
    'amount': '0.01',
    'subject': 'Python SDK 测试订单'
}

try:
    result = wepay.create_order(order_params)
    print(f"下单结果: {json.dumps(result, ensure_ascii=False, indent=2)}")
    
    if result.get('code') == 1:
        print(f"✓ 下单成功！")
        print(f"  平台订单号: {result.get('trade_no')}")
        print(f"  支付链接: {result.get('pay_url')}")
        trade_no = result.get('trade_no')
    else:
        print(f"✗ 下单失败: {result.get('msg')}")
except Exception as e:
    print(f"✗ 错误: {e}")

# ============ 查询订单 ============

print("\n[2] MD5 签名 - 查询订单")
print("-" * 80)

try:
    query_result = wepay.query_order(order_params['out_trade_no'])
    print(f"查询结果: {json.dumps(query_result, ensure_ascii=False, indent=2)}")
except Exception as e:
    print(f"✗ 错误: {e}")

# ============ RSA 签名示例 ============

print("\n[3] RSA 签名 - 统一下单")
print("-" * 80)

private_key_pem = """-----BEGIN PRIVATE KEY-----
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
-----END PRIVATE KEY-----"""

wepay_rsa = WePay(
    mch_id='M515637',
    mch_key='nca3twhvpqveixu2hdeutb6utpiet6k7',
    base_url='http://127.0.0.1:8088',
    sign_type='RSA',
    private_key_pem=private_key_pem
)

order_params_rsa = {
    'out_trade_no': f'order_rsa_py_{int(time.time() * 1000)}',
    'pay_type': 'alipay',
    'amount': '0.02',
    'subject': 'Python SDK RSA 签名测试订单'
}

try:
    result = wepay_rsa.create_order(order_params_rsa)
    print(f"下单结果: {json.dumps(result, ensure_ascii=False, indent=2)}")
    
    if result.get('code') == 1:
        print(f"✓ RSA 签名下单成功！")
        print(f"  平台订单号: {result.get('trade_no')}")
except Exception as e:
    print(f"✗ 错误: {e}")

# ============ 异步通知验证示例 ============

print("\n[4] 异步通知验证")
print("-" * 80)

# 模拟异步通知数据
notify_data = {
    'trade_no': 'W20260530152204174895',
    'out_trade_no': 'order_test_123',
    'amount': '0.01',
    'status': '1',
    'pay_type': 'wxpay'
}

# 生成签名
notify_sign = wepay.sign(notify_data)
print(f"生成的签名: {notify_sign}")

# 验证签名
is_valid = wepay.verify_notify(notify_data, notify_sign)
print(f"签名验证结果: {'✓ 有效' if is_valid else '✗ 无效'}")

# ============ 不同支付方式示例 ============

print("\n[5] 不同支付方式示例")
print("-" * 80)

pay_types = ['wxpay', 'alipay', 'qqpay']

for pay_type in pay_types:
    try:
        params = {
            'out_trade_no': f'order_{pay_type}_{int(time.time() * 1000)}',
            'pay_type': pay_type,
            'amount': '0.01',
            'subject': f'{pay_type} 测试订单'
        }
        result = wepay.create_order(params)
        if result.get('code') == 1:
            print(f"✓ {pay_type}: 下单成功 (订单号: {result.get('trade_no')})")
        else:
            print(f"✗ {pay_type}: {result.get('msg')}")
    except Exception as e:
        print(f"✗ {pay_type}: 错误 - {e}")

# ============ 错误处理示例 ============

print("\n[6] 错误处理示例")
print("-" * 80)

# 缺少必要参数
try:
    result = wepay.create_order({
        'out_trade_no': 'test_order',
        # 缺少 pay_type, amount, subject
    })
    print(f"结果: {result}")
except Exception as e:
    print(f"✓ 捕获异常: {e}")

# 无效的金额
try:
    result = wepay.create_order({
        'out_trade_no': 'test_order',
        'pay_type': 'wxpay',
        'amount': '-0.01',  # 无效的金额
        'subject': '测试'
    })
    print(f"结果: {result}")
except Exception as e:
    print(f"✓ 捕获异常: {e}")

print("\n" + "=" * 80)
print("示例完成！")
print("=" * 80)
