#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
WePay Python SDK
支持 MD5 和 RSA 签名的支付网关 SDK
"""

import requests
import json
import hashlib
import base64
from typing import Dict, Optional
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding
from cryptography.hazmat.backends import default_backend


class WePay:
    """WePay 支付网关 SDK"""

    def __init__(self, mch_id: str, mch_key: str, base_url: str = "http://127.0.0.1:8088",
                 sign_type: str = "MD5", private_key_pem: str = ""):
        """
        初始化 SDK

        Args:
            mch_id: 商户号
            mch_key: 商户密钥
            base_url: API 基础 URL
            sign_type: 签名方式 (MD5 或 RSA)
            private_key_pem: RSA 私钥 (仅当 sign_type=RSA 时需要)
        """
        self.mch_id = mch_id
        self.mch_key = mch_key
        self.base_url = base_url.rstrip('/')
        self.sign_type = sign_type.upper()
        self.private_key_pem = private_key_pem

    def sign(self, params: Dict) -> str:
        """
        生成签名

        Args:
            params: 参数字典

        Returns:
            签名值
        """
        if self.sign_type == 'RSA':
            return self._sign_rsa(params)
        else:
            return self._sign_md5(params)

    def _sign_md5(self, params: Dict) -> str:
        """MD5 签名"""
        sorted_params = sorted(params.items())
        sign_str = "&".join([f"{k}={v}" for k, v in sorted_params
                            if k not in ["sign", "sign_type"] and v])
        sign_str += self.mch_key
        return hashlib.md5(sign_str.encode()).hexdigest()

    def _sign_rsa(self, params: Dict) -> str:
        """RSA-SHA256 签名"""
        if not self.private_key_pem:
            raise ValueError("RSA 签名需要提供私钥")

        sorted_params = sorted(params.items())
        sign_str = "&".join([f"{k}={v}" for k, v in sorted_params
                            if k not in ["sign", "sign_type"] and v])

        private_key = serialization.load_pem_private_key(
            self.private_key_pem.encode(),
            password=None,
            backend=default_backend()
        )

        signature = private_key.sign(
            sign_str.encode(),
            padding.PKCS1v15(),
            hashes.SHA256()
        )

        return base64.b64encode(signature).decode()

    def create_order(self, params: Dict) -> Dict:
        """
        统一下单

        Args:
            params: 订单参数

        Returns:
            响应结果
        """
        params['mch_id'] = self.mch_id
        params['sign_type'] = self.sign_type
        params['sign'] = self.sign(params)

        return self._post('/gateway/create', params)

    def query_order(self, out_trade_no: str) -> Dict:
        """
        查询订单

        Args:
            out_trade_no: 商户订单号

        Returns:
            响应结果
        """
        params = {
            'mch_id': self.mch_id,
            'out_trade_no': out_trade_no,
            'sign_type': self.sign_type
        }
        params['sign'] = self.sign(params)

        return self._post('/gateway/query', params)

    def refund(self, trade_no: str, refund_amount: str) -> Dict:
        """
        申请退款

        Args:
            trade_no: 平台订单号
            refund_amount: 退款金额

        Returns:
            响应结果
        """
        params = {
            'mch_id': self.mch_id,
            'trade_no': trade_no,
            'refund_amount': refund_amount,
            'sign_type': self.sign_type
        }
        params['sign'] = self.sign(params)

        return self._post('/gateway/refund', params)

    def verify_notify(self, params: Dict, sign: str) -> bool:
        """
        验证异步通知签名

        Args:
            params: 通知参数
            sign: 签名值

        Returns:
            验证结果
        """
        calc_sign = self.sign(params)
        return calc_sign == sign

    def _post(self, path: str, params: Dict) -> Dict:
        """
        POST 请求

        Args:
            path: 接口路径
            params: 参数

        Returns:
            响应结果
        """
        url = self.base_url + path
        try:
            response = requests.post(
                url,
                json=params,
                headers={'Content-Type': 'application/json'},
                timeout=10
            )
            response.raise_for_status()
            return response.json()
        except requests.exceptions.RequestException as e:
            raise Exception(f"请求失败: {e}")


if __name__ == '__main__':
    # 示例代码
    wepay = WePay(
        'M515637',
        'nca3twhvpqveixu2hdeutb6utpiet6k7',
        'http://127.0.0.1:8088',
        'MD5'
    )

    # 创建订单
    order_params = {
        'out_trade_no': 'order_test_123',
        'pay_type': 'wxpay',
        'amount': '0.01',
        'subject': 'Python SDK 测试订单'
    }

    try:
        result = wepay.create_order(order_params)
        print("下单结果:")
        print(json.dumps(result, ensure_ascii=False, indent=2))
    except Exception as e:
        print(f"错误: {e}")
