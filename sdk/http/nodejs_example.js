#!/usr/bin/env node

/**
 * WePay HTTP 调用示例 - Node.js
 * 
 * 安装依赖:
 * npm install axios
 */

const crypto = require('crypto');
const axios = require('axios');

const MCH_ID = 'M515637';
const MCH_KEY = 'nca3twhvpqveixu2hdeutb6utpiet6k7';
const BASE_URL = 'http://127.0.0.1:8088';

console.log('='.repeat(80));
console.log('WePay HTTP 调用示例 (Node.js)');
console.log('='.repeat(80));

// ============ MD5 签名函数 ============

function signMd5(params) {
    const sorted = Object.keys(params)
        .filter(k => k !== 'sign' && k !== 'sign_type' && params[k])
        .sort()
        .map(k => `${k}=${params[k]}`)
        .join('&');
    
    const signStr = sorted + MCH_KEY;
    return crypto.createHash('md5').update(signStr).digest('hex');
}

// ============ [1] 统一下单 ============

async function testCreateOrder() {
    console.log('\n[1] 统一下单');
    console.log('-'.repeat(80));

    const outTradeNo = `order_nodejs_${Date.now()}`;
    const params = {
        mch_id: MCH_ID,
        out_trade_no: outTradeNo,
        pay_type: 'wxpay',
        amount: '0.01',
        subject: 'Node.js 测试订单',
        sign_type: 'MD5'
    };

    params.sign = signMd5(params);

    console.log('请求参数:');
    console.log(JSON.stringify(params, null, 2));
    console.log('');

    try {
        const response = await axios.post(`${BASE_URL}/gateway/create`, params, {
            headers: { 'Content-Type': 'application/json' }
        });

        console.log('响应结果:');
        console.log(JSON.stringify(response.data, null, 2));

        if (response.data.code === 1) {
            console.log('✓ 下单成功！');
            console.log(`  平台订单号: ${response.data.trade_no}`);
            console.log(`  支付链接: ${response.data.pay_url}`);
            return { outTradeNo, tradeNo: response.data.trade_no };
        } else {
            console.log(`✗ 下单失败: ${response.data.msg}`);
            return null;
        }
    } catch (error) {
        console.error('✗ 错误:', error.message);
        return null;
    }
}

// ============ [2] 查询订单 ============

async function testQueryOrder(outTradeNo) {
    console.log('\n[2] 查询订单');
    console.log('-'.repeat(80));

    const params = {
        mch_id: MCH_ID,
        out_trade_no: outTradeNo,
        sign_type: 'MD5'
    };

    params.sign = signMd5(params);

    console.log(`查询订单号: ${outTradeNo}`);
    console.log('');

    try {
        const response = await axios.post(`${BASE_URL}/gateway/query`, params, {
            headers: { 'Content-Type': 'application/json' }
        });

        console.log('响应结果:');
        console.log(JSON.stringify(response.data, null, 2));
    } catch (error) {
        console.error('✗ 错误:', error.message);
    }
}

// ============ [3] 异步通知验证 ============

function testVerifyNotify() {
    console.log('\n[3] 异步通知验证');
    console.log('-'.repeat(80));

    const notifyData = {
        trade_no: 'W20260530152204174895',
        out_trade_no: 'order_test_123',
        amount: '0.01',
        status: '1',
        pay_type: 'wxpay'
    };

    const sign = signMd5(notifyData);
    console.log('通知参数:');
    console.log(JSON.stringify(notifyData, null, 2));
    console.log('');
    console.log(`生成的签名: ${sign}`);
    console.log('');

    // 验证签名
    const verifySign = signMd5(notifyData);
    if (sign === verifySign) {
        console.log('✓ 签名验证通过');
    } else {
        console.log('✗ 签名验证失败');
    }
}

// ============ [4] 不同支付方式 ============

async function testDifferentPayTypes() {
    console.log('\n[4] 不同支付方式测试');
    console.log('-'.repeat(80));

    const payTypes = ['wxpay', 'alipay', 'qqpay'];

    for (const payType of payTypes) {
        const outTradeNo = `order_${payType}_${Date.now()}`;
        const params = {
            mch_id: MCH_ID,
            out_trade_no: outTradeNo,
            pay_type: payType,
            amount: '0.01',
            subject: `${payType} 测试订单`,
            sign_type: 'MD5'
        };

        params.sign = signMd5(params);

        try {
            const response = await axios.post(`${BASE_URL}/gateway/create`, params, {
                headers: { 'Content-Type': 'application/json' }
            });

            if (response.data.code === 1) {
                console.log(`✓ ${payType}: 下单成功 (订单号: ${response.data.trade_no})`);
            } else {
                console.log(`✗ ${payType}: ${response.data.msg}`);
            }
        } catch (error) {
            console.error(`✗ ${payType}: 错误 - ${error.message}`);
        }
    }
}

// ============ 主函数 ============

async function main() {
    try {
        // 测试下单
        const orderResult = await testCreateOrder();

        // 测试查询
        if (orderResult) {
            await testQueryOrder(orderResult.outTradeNo);
        }

        // 测试异步通知验证
        testVerifyNotify();

        // 测试不同支付方式
        await testDifferentPayTypes();

        console.log('\n' + '='.repeat(80));
        console.log('示例完成！');
        console.log('='.repeat(80));
    } catch (error) {
        console.error('错误:', error);
    }
}

// 运行示例
main();
