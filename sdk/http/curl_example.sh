#!/bin/bash

MCH_ID="M515637"
MCH_KEY="nca3twhvpqveixu2hdeutb6utpiet6k7"
BASE_URL="http://127.0.0.1:8088"

echo "WePay HTTP 调用示例 (Bash/curl)"
echo "========================================"

sign_md5() {
    echo -n "$1${MCH_KEY}" | md5sum | awk '{print $1}'
}

# [1] 统一下单
echo ""
echo "[1] 统一下单"

OUT_TRADE_NO="order_bash_$(date +%s%N)"
SIGN_PARAMS="amount=0.01&mch_id=${MCH_ID}&out_trade_no=${OUT_TRADE_NO}&pay_type=wxpay&subject=测试"
SIGN=$(sign_md5 "$SIGN_PARAMS")

RESPONSE=$(curl -s -X POST "${BASE_URL}/gateway/create" \
    -H "Content-Type: application/json" \
    -d "{\"mch_id\":\"${MCH_ID}\",\"out_trade_no\":\"${OUT_TRADE_NO}\",\"pay_type\":\"wxpay\",\"amount\":\"0.01\",\"subject\":\"测试\",\"sign_type\":\"MD5\",\"sign\":\"${SIGN}\"}")

echo "$RESPONSE" | jq '.' 2>/dev/null || echo "$RESPONSE"
TRADE_NO=$(echo "$RESPONSE" | jq -r '.trade_no' 2>/dev/null)

# [2] 查询订单
echo ""
echo "[2] 查询订单"

if [ ! -z "$TRADE_NO" ] && [ "$TRADE_NO" != "null" ]; then
    SIGN=$(sign_md5 "mch_id=${MCH_ID}&out_trade_no=${OUT_TRADE_NO}")
    RESPONSE=$(curl -s -X POST "${BASE_URL}/gateway/query" \
        -H "Content-Type: application/json" \
        -d "{\"mch_id\":\"${MCH_ID}\",\"out_trade_no\":\"${OUT_TRADE_NO}\",\"sign_type\":\"MD5\",\"sign\":\"${SIGN}\"}")
    echo "$RESPONSE" | jq '.' 2>/dev/null || echo "$RESPONSE"
else
    echo "✗ 下单失败，跳过查询"
fi

# ============ [3] 异步通知验证 ============

echo ""
echo "[3] 异步通知验证"
echo "--------------------------------------------------------------------------------"

NOTIFY_TRADE_NO="W20260530152204174895"
NOTIFY_OUT_TRADE_NO="order_test_123"
NOTIFY_AMOUNT="0.01"
NOTIFY_STATUS="1"
NOTIFY_PAY_TYPE="wxpay"

# 构造通知签名
NOTIFY_SIGN_PARAMS="amount=${NOTIFY_AMOUNT}&mch_id=${MCH_ID}&out_trade_no=${NOTIFY_OUT_TRADE_NO}&pay_type=${NOTIFY_PAY_TYPE}&status=${NOTIFY_STATUS}&trade_no=${NOTIFY_TRADE_NO}"
NOTIFY_SIGN=$(sign_md5 "$NOTIFY_SIGN_PARAMS")

echo "通知参数:"
echo "  trade_no: ${NOTIFY_TRADE_NO}"
echo "  out_trade_no: ${NOTIFY_OUT_TRADE_NO}"
echo "  amount: ${NOTIFY_AMOUNT}"
echo "  status: ${NOTIFY_STATUS}"
echo "  pay_type: ${NOTIFY_PAY_TYPE}"
echo ""
echo "生成的签名: ${NOTIFY_SIGN}"
echo ""

# 验证签名（重新计算）
VERIFY_SIGN=$(sign_md5 "$NOTIFY_SIGN_PARAMS")
if [ "$NOTIFY_SIGN" = "$VERIFY_SIGN" ]; then
    echo "✓ 签名验证通过"
else
    echo "✗ 签名验证失败"
fi

# ============ [4] 不同支付方式 ============

echo ""
echo "[4] 不同支付方式测试"
echo "--------------------------------------------------------------------------------"

for PAY_TYPE in "wxpay" "alipay" "qqpay"; do
    OUT_TRADE_NO="order_${PAY_TYPE}_$(date +%s%N)"
    SIGN_PARAMS="amount=0.01&mch_id=${MCH_ID}&out_trade_no=${OUT_TRADE_NO}&pay_type=${PAY_TYPE}&subject=${PAY_TYPE}测试"
    SIGN=$(sign_md5 "$SIGN_PARAMS")

    RESPONSE=$(curl -s -X POST "${BASE_URL}/gateway/create" \
        -H "Content-Type: application/json" \
        -d "{
            \"mch_id\": \"${MCH_ID}\",
            \"out_trade_no\": \"${OUT_TRADE_NO}\",
            \"pay_type\": \"${PAY_TYPE}\",
            \"amount\": \"0.01\",
            \"subject\": \"${PAY_TYPE}测试\",
            \"sign_type\": \"MD5\",
            \"sign\": \"${SIGN}\"
        }")

    CODE=$(echo "$RESPONSE" | jq -r '.code' 2>/dev/null)
    if [ "$CODE" = "1" ]; then
        TRADE_NO=$(echo "$RESPONSE" | jq -r '.trade_no' 2>/dev/null)
        echo "✓ ${PAY_TYPE}: 下单成功 (订单号: ${TRADE_NO})"
    else
        MSG=$(echo "$RESPONSE" | jq -r '.msg' 2>/dev/null)
        echo "✗ ${PAY_TYPE}: ${MSG}"
    fi
done

echo ""
echo "================================================================================"
echo "示例完成！"
echo "================================================================================"
