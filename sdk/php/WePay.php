<?php
/**
 * WePay PHP SDK
 * 支持 MD5 和 RSA 签名的支付网关 SDK
 */

class WePay {
    private $mchId;
    private $mchKey;
    private $privateKeyPem;
    private $baseUrl;
    private $signType;

    /**
     * 初始化 SDK
     * @param string $mchId 商户号
     * @param string $mchKey 商户密钥
     * @param string $baseUrl API 基础 URL
     * @param string $signType 签名方式 (MD5 或 RSA)
     * @param string $privateKeyPem RSA 私钥 (仅当 signType=RSA 时需要)
     */
    public function __construct($mchId, $mchKey, $baseUrl = "http://127.0.0.1:8088", $signType = "MD5", $privateKeyPem = "") {
        $this->mchId = $mchId;
        $this->mchKey = $mchKey;
        $this->baseUrl = rtrim($baseUrl, '/');
        $this->signType = strtoupper($signType);
        $this->privateKeyPem = $privateKeyPem;
    }

    /**
     * 生成签名
     * @param array $params 参数数组
     * @return string 签名值
     */
    public function sign($params) {
        if ($this->signType === 'RSA') {
            return $this->signRsa($params);
        } else {
            return $this->signMd5($params);
        }
    }

    /**
     * MD5 签名
     * @param array $params 参数数组
     * @return string 签名值
     */
    private function signMd5($params) {
        ksort($params);
        $str = '';
        foreach ($params as $k => $v) {
            if ($k !== 'sign' && $k !== 'sign_type' && !empty($v)) {
                $str .= $k . '=' . $v . '&';
            }
        }
        $str = rtrim($str, '&');
        $str .= $this->mchKey;
        return md5($str);
    }

    /**
     * RSA-SHA256 签名
     * @param array $params 参数数组
     * @return string Base64 编码的签名值
     */
    private function signRsa($params) {
        if (empty($this->privateKeyPem)) {
            throw new Exception("RSA 签名需要提供私钥");
        }

        ksort($params);
        $str = '';
        foreach ($params as $k => $v) {
            if ($k !== 'sign' && $k !== 'sign_type' && !empty($v)) {
                $str .= $k . '=' . $v . '&';
            }
        }
        $str = rtrim($str, '&');

        $privateKey = openssl_pkey_get_private($this->privateKeyPem);
        if (!$privateKey) {
            throw new Exception("私钥格式错误");
        }

        $signature = '';
        if (!openssl_sign($str, $signature, $privateKey, OPENSSL_ALGO_SHA256)) {
            throw new Exception("签名失败");
        }
        openssl_free_key($privateKey);

        return base64_encode($signature);
    }

    /**
     * 统一下单
     * @param array $params 订单参数
     * @return array 响应结果
     */
    public function createOrder($params) {
        $params['mch_id'] = $this->mchId;
        $params['sign_type'] = $this->signType;
        $params['sign'] = $this->sign($params);

        return $this->post('/gateway/create', $params);
    }

    /**
     * 查询订单
     * @param string $outTradeNo 商户订单号
     * @return array 响应结果
     */
    public function queryOrder($outTradeNo) {
        $params = [
            'mch_id' => $this->mchId,
            'out_trade_no' => $outTradeNo,
            'sign_type' => $this->signType
        ];
        $params['sign'] = $this->sign($params);

        return $this->post('/gateway/query', $params);
    }

    /**
     * 申请退款
     * @param string $tradeNo 平台订单号
     * @param string $refundAmount 退款金额
     * @return array 响应结果
     */
    public function refund($tradeNo, $refundAmount) {
        $params = [
            'mch_id' => $this->mchId,
            'trade_no' => $tradeNo,
            'refund_amount' => $refundAmount,
            'sign_type' => $this->signType
        ];
        $params['sign'] = $this->sign($params);

        return $this->post('/gateway/refund', $params);
    }

    /**
     * 验证异步通知签名
     * @param array $params 通知参数
     * @param string $sign 签名值
     * @return bool 验证结果
     */
    public function verifyNotify($params, $sign) {
        $calcSign = $this->sign($params);
        return $calcSign === $sign;
    }

    /**
     * POST 请求
     * @param string $path 接口路径
     * @param array $params 参数
     * @return array 响应结果
     */
    private function post($path, $params) {
        $url = $this->baseUrl . $path;
        $ch = curl_init();
        curl_setopt($ch, CURLOPT_URL, $url);
        curl_setopt($ch, CURLOPT_POST, true);
        curl_setopt($ch, CURLOPT_POSTFIELDS, json_encode($params));
        curl_setopt($ch, CURLOPT_HTTPHEADER, ['Content-Type: application/json']);
        curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
        curl_setopt($ch, CURLOPT_TIMEOUT, 10);

        $response = curl_exec($ch);
        $httpCode = curl_getinfo($ch, CURLINFO_HTTP_CODE);
        curl_close($ch);

        if ($httpCode !== 200) {
            throw new Exception("HTTP 错误: " . $httpCode);
        }

        return json_decode($response, true);
    }
}
?>
