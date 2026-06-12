using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using WePay;
using Newtonsoft.Json;

/**
 * WePay C# SDK 使用示例
 */
class Program
{
    static async Task Main(string[] args)
    {
        Console.WriteLine("================================================================================");
        Console.WriteLine("WePay C# SDK 示例");
        Console.WriteLine("================================================================================");

        // ============ MD5 签名示例 ============

        Console.WriteLine("\n[1] MD5 签名 - 统一下单");
        Console.WriteLine("--------------------------------------------------------------------------------");

        try
        {
            var wepay = new WePayClient(
                "M515637",
                "nca3twhvpqveixu2hdeutb6utpiet6k7",
                "http://127.0.0.1:8088",
                "MD5"
            );

            var orderParams = new Dictionary<string, string>
            {
                { "out_trade_no", "order_csharp_" + DateTimeOffset.Now.ToUnixTimeMilliseconds() },
                { "pay_type", "wxpay" },
                { "amount", "0.01" },
                { "subject", "C# SDK 测试订单" }
            };

            var result = await wepay.CreateOrderAsync(orderParams);
            Console.WriteLine("下单结果: " + JsonConvert.SerializeObject(result, Formatting.Indented));

            if (result.ContainsKey("code") && result["code"].ToString() == "1")
            {
                Console.WriteLine("✓ 下单成功！");
                Console.WriteLine($"  平台订单号: {result["trade_no"]}");
                Console.WriteLine($"  支付链接: {result["pay_url"]}");
            }
            else
            {
                Console.WriteLine($"✗ 下单失败: {result["msg"]}");
            }
        }
        catch (Exception e)
        {
            Console.WriteLine($"✗ 错误: {e.Message}");
        }

        // ============ 查询订单 ============

        Console.WriteLine("\n[2] MD5 签名 - 查询订单");
        Console.WriteLine("--------------------------------------------------------------------------------");

        try
        {
            var wepay = new WePayClient(
                "M515637",
                "nca3twhvpqveixu2hdeutb6utpiet6k7",
                "http://127.0.0.1:8088",
                "MD5"
            );

            var queryResult = await wepay.QueryOrderAsync("order_csharp_test_123");
            Console.WriteLine("查询结果: " + JsonConvert.SerializeObject(queryResult, Formatting.Indented));
        }
        catch (Exception e)
        {
            Console.WriteLine($"✗ 错误: {e.Message}");
        }

        // ============ 不同支付方式示例 ============

        Console.WriteLine("\n[3] 不同支付方式示例");
        Console.WriteLine("--------------------------------------------------------------------------------");

        string[] payTypes = { "wxpay", "alipay", "qqpay" };

        try
        {
            var wepay = new WePayClient(
                "M515637",
                "nca3twhvpqveixu2hdeutb6utpiet6k7",
                "http://127.0.0.1:8088",
                "MD5"
            );

            foreach (var payType in payTypes)
            {
                var params_dict = new Dictionary<string, string>
                {
                    { "out_trade_no", $"order_{payType}_{DateTimeOffset.Now.ToUnixTimeMilliseconds()}" },
                    { "pay_type", payType },
                    { "amount", "0.01" },
                    { "subject", $"{payType} 测试订单" }
                };

                try
                {
                    var result = await wepay.CreateOrderAsync(params_dict);
                    if (result.ContainsKey("code") && result["code"].ToString() == "1")
                    {
                        Console.WriteLine($"✓ {payType}: 下单成功 (订单号: {result["trade_no"]})");
                    }
                    else
                    {
                        Console.WriteLine($"✗ {payType}: {result["msg"]}");
                    }
                }
                catch (Exception e)
                {
                    Console.WriteLine($"✗ {payType}: 错误 - {e.Message}");
                }
            }
        }
        catch (Exception e)
        {
            Console.WriteLine($"✗ 错误: {e.Message}");
        }

        // ============ 异步通知验证示例 ============

        Console.WriteLine("\n[4] 异步通知验证");
        Console.WriteLine("--------------------------------------------------------------------------------");

        try
        {
            var wepay = new WePayClient(
                "M515637",
                "nca3twhvpqveixu2hdeutb6utpiet6k7",
                "http://127.0.0.1:8088",
                "MD5"
            );

            var notifyData = new Dictionary<string, string>
            {
                { "trade_no", "W20260530152204174895" },
                { "out_trade_no", "order_test_123" },
                { "amount", "0.01" },
                { "status", "1" },
                { "pay_type", "wxpay" }
            };

            var sign = wepay.Sign(notifyData);
            Console.WriteLine($"生成的签名: {sign}");

            var isValid = wepay.VerifyNotify(notifyData, sign);
            Console.WriteLine($"签名验证结果: {(isValid ? "✓ 有效" : "✗ 无效")}");
        }
        catch (Exception e)
        {
            Console.WriteLine($"✗ 错误: {e.Message}");
        }

        // ============ 错误处理示例 ============

        Console.WriteLine("\n[5] 错误处理示例");
        Console.WriteLine("--------------------------------------------------------------------------------");

        try
        {
            var wepay = new WePayClient(
                "M515637",
                "nca3twhvpqveixu2hdeutb6utpiet6k7",
                "http://127.0.0.1:8088",
                "MD5"
            );

            // 缺少必要参数
            var invalidParams = new Dictionary<string, string>
            {
                { "out_trade_no", "test_order" }
                // 缺少 pay_type, amount, subject
            };

            var result = await wepay.CreateOrderAsync(invalidParams);
            Console.WriteLine("结果: " + JsonConvert.SerializeObject(result));
        }
        catch (Exception e)
        {
            Console.WriteLine($"✓ 捕获异常: {e.Message}");
        }

        // ============ RSA 签名示例 ============

        Console.WriteLine("\n[6] RSA 签名 - 统一下单");
        Console.WriteLine("--------------------------------------------------------------------------------");

        string privateKeyPem = @"-----BEGIN PRIVATE KEY-----
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
-----END PRIVATE KEY-----";

        try
        {
            var wePayRsa = new WePayClient(
                "M515637",
                "nca3twhvpqveixu2hdeutb6utpiet6k7",
                "http://127.0.0.1:8088",
                "RSA",
                privateKeyPem
            );

            var orderParamsRsa = new Dictionary<string, string>
            {
                { "out_trade_no", "order_rsa_csharp_" + DateTimeOffset.Now.ToUnixTimeMilliseconds() },
                { "pay_type", "alipay" },
                { "amount", "0.02" },
                { "subject", "C# SDK RSA 签名测试订单" }
            };

            var result = await wePayRsa.CreateOrderAsync(orderParamsRsa);
            Console.WriteLine("下单结果: " + JsonConvert.SerializeObject(result, Formatting.Indented));

            if (result.ContainsKey("code") && result["code"].ToString() == "1")
            {
                Console.WriteLine("✓ RSA 签名下单成功！");
                Console.WriteLine($"  平台订单号: {result["trade_no"]}");
            }
            else
            {
                Console.WriteLine($"✗ 下单失败: {result["msg"]}");
            }
        }
        catch (Exception e)
        {
            Console.WriteLine($"✗ 错误: {e.Message}");
            Console.WriteLine(e.StackTrace);
        }

        Console.WriteLine("\n================================================================================");
        Console.WriteLine("示例完成！");
        Console.WriteLine("================================================================================");
    }
}
