package main

import (
	"encoding/json"
	"fmt"
	"log"
	"strconv"
	"time"
	"wepay"
)

func main() {
	fmt.Println(string([]byte{61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61}))
	fmt.Println("WePay Go SDK 示例")
	fmt.Println(string([]byte{61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61}))

	// ============ MD5 签名示例 ============

	fmt.Println("\n[1] MD5 签名 - 统一下单")
	fmt.Println(string([]byte{45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45}))

	client, err := wepay.New(
		"M515637",
		"nca3twhvpqveixu2hdeutb6utpiet6k7",
		"http://127.0.0.1:8088",
		"MD5",
		"",
	)
	if err != nil {
		log.Fatal(err)
	}

	orderParams := map[string]string{
		"out_trade_no": "order_go_" + strconv.FormatInt(time.Now().UnixMilli(), 10),
		"pay_type":     "wxpay",
		"amount":       "0.01",
		"subject":      "Go SDK 测试订单",
	}

	result, err := client.CreateOrder(orderParams)
	if err != nil {
		log.Fatal(err)
	}

	jsonData, _ := json.MarshalIndent(result, "", "  ")
	fmt.Printf("下单结果: %s\n", string(jsonData))

	if result["code"].(float64) == 1 {
		fmt.Printf("✓ 下单成功！\n")
		fmt.Printf("  平台订单号: %v\n", result["trade_no"])
		fmt.Printf("  支付链接: %v\n", result["pay_url"])
	}

	// ============ 查询订单 ============

	fmt.Println("\n[2] MD5 签名 - 查询订单")
	fmt.Println(string([]byte{45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45}))

	queryResult, err := client.QueryOrder(orderParams["out_trade_no"])
	if err != nil {
		log.Fatal(err)
	}

	jsonData, _ = json.MarshalIndent(queryResult, "", "  ")
	fmt.Printf("查询结果: %s\n", string(jsonData))

	// ============ 不同支付方式示例 ============

	fmt.Println("\n[3] 不同支付方式示例")
	fmt.Println(string([]byte{45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45}))

	payTypes := []string{"wxpay", "alipay", "qqpay"}

	for _, payType := range payTypes {
		params := map[string]string{
			"out_trade_no": "order_" + payType + "_" + strconv.FormatInt(time.Now().UnixMilli(), 10),
			"pay_type":     payType,
			"amount":       "0.01",
			"subject":      payType + " 测试订单",
		}

		result, err := client.CreateOrder(params)
		if err != nil {
			fmt.Printf("✗ %s: 错误 - %v\n", payType, err)
			continue
		}

		if result["code"].(float64) == 1 {
			fmt.Printf("✓ %s: 下单成功 (订单号: %v)\n", payType, result["trade_no"])
		} else {
			fmt.Printf("✗ %s: %v\n", payType, result["msg"])
		}
	}

	// ============ 异步通知验证示例 ============

	fmt.Println("\n[4] 异步通知验证")
	fmt.Println(string([]byte{45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45, 45}))

	notifyData := map[string]string{
		"trade_no":     "W20260530152204174895",
		"out_trade_no": "order_test_123",
		"amount":       "0.01",
		"status":       "1",
		"pay_type":     "wxpay",
	}

	sign, _ := client.Sign(notifyData)
	fmt.Printf("生成的签名: %s\n", sign)

	isValid, _ := client.VerifyNotify(notifyData, sign)
	if isValid {
		fmt.Println("✓ 签名验证通过")
	} else {
		fmt.Println("✗ 签名验证失败")
	}

	fmt.Println("\n" + string([]byte{61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61}))
	fmt.Println("示例完成！")
	fmt.Println(string([]byte{61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61, 61}))
}
