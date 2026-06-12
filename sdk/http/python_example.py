#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
WePay HTTP 调用示例 - Python requests

安装依赖:
pip install requests
"""

import requests
import hashlib
import json
import time
from datetime import datetime

MCH_ID = 'M515637'
MCH_KEY = 'nca3twhvpqveixu2hdeutb6utpiet6k7'
BASE_URL = 'http://127.0.0.1:8088'

print('=' * 80)
print('WePay HTTP 调用示例 (Python requests)')
print('=' * 80)

# ============ MD5 签名函数 ============

def sign_md5(params):
    """生成 MD5 签名"""
    sorted_params = sorted([
        (k, v) for k, v in params.items() 
        if k not in ['sign', 'sign_type'] and v
    ])
    sign_str = '&'.join([f'{k}={v}' for k, v in sorted_params])
    sign_str += MCH_KEY
    return hashlib.md5(sign_str.encode()).hexdigest()

# ============ [1] 统一下单 ============

print('\n[1] 统一下单')
print('-' * 80)

out_trade_no = f'order_py_{int(time.time() * 1000)}'
params = {
    'mch_id': MCH_ID,
    'out_trade_no': out_trade_no,
    'pay_type': 'wxpay',
    'amount': '0.01',
    'subject': 'Python requests 测试订单',
    'sign_type': 'MD5'
}

params['sign'] = sign_md5(params)

print('请求参数:')
print(json.dumps(params, ensure_ascii=False, indent=2))
print('')

try:
    response = requests.post(
        f'{BASE_URL}/gateway/create',
        json=params,
        headers={'Content-Type': 'application/json'},
        timeout=10
    )

    result = response.json()
    print('响应结果:')
    print(json.dumps(result, ensure_ascii=False, indent=2))

    if result.get('code') == 1:
        print('✓ 下单成功！')
        print(f"  平台订单号: {result.get('trade_no')}")
        print(f"  支付链接: {result.get('pay_url')}")
        trade_no = result.get('trade_no')
    else:
        print(f"✗ 下单失败: {result.get('msg')}")
        trade_no = None
except Exception as e:
    print(f'✗ 错误: {e}')
    trade_no = None

# ============ [2] 查询订单 ============

print('\n[2] 查询订单')
print('-' * 80)

if trade_no:
    query_params = {
        'mch_id': MCH_ID,
        'out_trade_no': out_trade_no,
        'sign_type': 'MD5'
    }

    query_params['sign'] = sign_md5(query_params)

    print(f'查询订单号: {out_trade_no}')
    print('')

    try:
        response = requests.post(
            f'{BASE_URL}/gateway/query',
            json=query_params,
            headers={'Content-Type': 'application/json'},
            timeout=10
        )

        result = response.json()
        print('响应结果:')
        print(json.dumps(result, ensure_ascii=False, indent=2))
    except Exception as e:
        print(f'✗ 错误: {e}')
else:
    print('✗ 下单失败，跳过查询')

# ============ [3] 异步通知验证 ============

print('\n[3] 异步通知验证')
print('-' * 80)

notify_data = {
    'trade_no': 'W20260530152204174895',
    'out_trade_no': 'order_test_123',
    'amount': '0.01',
    'status': '1',
    'pay_type': 'wxpay'
}

notify_sign = sign_md5(notify_data)
print('通知参数:')
print(json.dumps(notify_data, ensure_ascii=False, indent=2))
print('')
print(f'生成的签名: {notify_sign}')
print('')

# 验证签名
verify_sign = sign_md5(notify_data)
if notify_sign == verify_sign:
    print('✓ 签名验证通过')
else:
    print('✗ 签名验证失败')

# ============ [4] 不同支付方式 ============

print('\n[4] 不同支付方式测试')
print('-' * 80)

pay_types = ['wxpay', 'alipay', 'qqpay']

for pay_type in pay_types:
    out_trade_no = f'order_{pay_type}_{int(time.time() * 1000)}'
    params = {
        'mch_id': MCH_ID,
        'out_trade_no': out_trade_no,
        'pay_type': pay_type,
        'amount': '0.01',
        'subject': f'{pay_type} 测试订单',
        'sign_type': 'MD5'
    }

    params['sign'] = sign_md5(params)

    try:
        response = requests.post(
            f'{BASE_URL}/gateway/create',
            json=params,
            headers={'Content-Type': 'application/json'},
            timeout=10
        )

        result = response.json()
        if result.get('code') == 1:
            print(f"✓ {pay_type}: 下单成功 (订单号: {result.get('trade_no')})")
        else:
            print(f"✗ {pay_type}: {result.get('msg')}")
    except Exception as e:
        print(f'✗ {pay_type}: 错误 - {e}')

# ============ [5] 错误处理示例 ============

print('\n[5] 错误处理示例')
print('-' * 80)

# 缺少必要参数
invalid_params = {
    'mch_id': MCH_ID,
    'out_trade_no': 'test_order',
    # 缺少 pay_type, amount, subject
    'sign_type': 'MD5'
}

invalid_params['sign'] = sign_md5(invalid_params)

print('发送缺少参数的请求...')
print('')

try:
    response = requests.post(
        f'{BASE_URL}/gateway/create',
        json=invalid_params,
        headers={'Content-Type': 'application/json'},
        timeout=10
    )

    result = response.json()
    if result.get('code') != 1:
        print(f"✓ 捕获错误: {result.get('msg')}")
    else:
        print('响应:', json.dumps(result, ensure_ascii=False, indent=2))
except Exception as e:
    print(f'✗ 异常: {e}')

print('\n' + '=' * 80)
print('示例完成！')
print('=' * 80)
