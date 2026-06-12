package wepay

import (
	"bytes"
	"crypto"
	"crypto/md5"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"crypto/x509"
	"encoding/base64"
	"encoding/json"
	"encoding/pem"
	"fmt"
	"io"
	"net/http"
	"sort"
	"strings"
	"time"
)

// WePay SDK 结构体
type WePay struct {
	MchID       string
	MchKey      string
	BaseURL     string
	SignType    string
	PrivateKey  *rsa.PrivateKey
	HTTPClient  *http.Client
}

// 创建新的 WePay SDK 实例
func New(mchID, mchKey, baseURL, signType, privateKeyPEM string) (*WePay, error) {
	w := &WePay{
		MchID:      mchID,
		MchKey:     mchKey,
		BaseURL:    strings.TrimRight(baseURL, "/"),
		SignType:   strings.ToUpper(signType),
		HTTPClient: &http.Client{Timeout: 10 * time.Second},
	}

	if w.SignType == "RSA" && privateKeyPEM != "" {
		privateKey, err := loadPrivateKey(privateKeyPEM)
		if err != nil {
			return nil, err
		}
		w.PrivateKey = privateKey
	}

	return w, nil
}

// 加载 RSA 私钥
func loadPrivateKey(privateKeyPEM string) (*rsa.PrivateKey, error) {
	block, _ := pem.Decode([]byte(privateKeyPEM))
	if block == nil {
		return nil, fmt.Errorf("私钥格式错误")
	}

	privateKey, err := x509.ParsePKCS8PrivateKey(block.Bytes)
	if err != nil {
		return nil, err
	}

	rsaKey, ok := privateKey.(*rsa.PrivateKey)
	if !ok {
		return nil, fmt.Errorf("不是 RSA 私钥")
	}

	return rsaKey, nil
}

// 生成签名
func (w *WePay) Sign(params map[string]string) (string, error) {
	if w.SignType == "RSA" {
		return w.signRSA(params)
	}
	return w.signMD5(params), nil
}

// MD5 签名
func (w *WePay) signMD5(params map[string]string) string {
	// 排序参数
	keys := make([]string, 0, len(params))
	for k := range params {
		if k != "sign" && k != "sign_type" && params[k] != "" {
			keys = append(keys, k)
		}
	}
	sort.Strings(keys)

	// 构建签名字符串
	var sb strings.Builder
	for i, k := range keys {
		if i > 0 {
			sb.WriteString("&")
		}
		sb.WriteString(k)
		sb.WriteString("=")
		sb.WriteString(params[k])
	}
	sb.WriteString(w.MchKey)

	// MD5 签名
	hash := md5.Sum([]byte(sb.String()))
	return fmt.Sprintf("%x", hash)
}

// RSA-SHA256 签名
func (w *WePay) signRSA(params map[string]string) (string, error) {
	if w.PrivateKey == nil {
		return "", fmt.Errorf("RSA 签名需要提供私钥")
	}

	// 排序参数
	keys := make([]string, 0, len(params))
	for k := range params {
		if k != "sign" && k != "sign_type" && params[k] != "" {
			keys = append(keys, k)
		}
	}
	sort.Strings(keys)

	// 构建签名字符串
	var sb strings.Builder
	for i, k := range keys {
		if i > 0 {
			sb.WriteString("&")
		}
		sb.WriteString(k)
		sb.WriteString("=")
		sb.WriteString(params[k])
	}

	// RSA-SHA256 签名
	hash := sha256.Sum256([]byte(sb.String()))
	signature, err := rsa.SignPKCS1v15(rand.Reader, w.PrivateKey, crypto.SHA256, hash[:])
	if err != nil {
		return "", err
	}

	return base64.StdEncoding.EncodeToString(signature), nil
}

// 统一下单
func (w *WePay) CreateOrder(params map[string]string) (map[string]interface{}, error) {
	params["mch_id"] = w.MchID
	params["sign_type"] = w.SignType

	sign, err := w.Sign(params)
	if err != nil {
		return nil, err
	}
	params["sign"] = sign

	return w.post("/gateway/create", params)
}

// 查询订单
func (w *WePay) QueryOrder(outTradeNo string) (map[string]interface{}, error) {
	params := map[string]string{
		"mch_id":       w.MchID,
		"out_trade_no": outTradeNo,
		"sign_type":    w.SignType,
	}

	sign, err := w.Sign(params)
	if err != nil {
		return nil, err
	}
	params["sign"] = sign

	return w.post("/gateway/query", params)
}

// 申请退款
func (w *WePay) Refund(tradeNo, refundAmount string) (map[string]interface{}, error) {
	params := map[string]string{
		"mch_id":         w.MchID,
		"trade_no":       tradeNo,
		"refund_amount":  refundAmount,
		"sign_type":      w.SignType,
	}

	sign, err := w.Sign(params)
	if err != nil {
		return nil, err
	}
	params["sign"] = sign

	return w.post("/gateway/refund", params)
}

// 验证异步通知签名
func (w *WePay) VerifyNotify(params map[string]string, sign string) (bool, error) {
	calcSign, err := w.Sign(params)
	if err != nil {
		return false, err
	}
	return calcSign == sign, nil
}

// POST 请求
func (w *WePay) post(path string, params map[string]string) (map[string]interface{}, error) {
	url := w.BaseURL + path

	jsonData, err := json.Marshal(params)
	if err != nil {
		return nil, err
	}

	req, err := http.NewRequest("POST", url, bytes.NewBuffer(jsonData))
	if err != nil {
		return nil, err
	}

	req.Header.Set("Content-Type", "application/json")

	resp, err := w.HTTPClient.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	if resp.StatusCode != 200 {
		return nil, fmt.Errorf("HTTP 错误: %d", resp.StatusCode)
	}

	body, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, err
	}

	var result map[string]interface{}
	if err := json.Unmarshal(body, &result); err != nil {
		return nil, err
	}

	return result, nil
}
