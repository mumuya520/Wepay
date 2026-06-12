import java.util.*;
import com.google.gson.Gson;
import com.google.gson.JsonObject;

/**
 * WePay Java SDK 使用示例
 */
public class Example {
    public static void main(String[] args) {
        System.out.println("================================================================================");
        System.out.println("WePay Java SDK 示例");
        System.out.println("================================================================================");

        // ============ MD5 签名示例 ============

        System.out.println("\n[1] MD5 签名 - 统一下单");
        System.out.println("--------------------------------------------------------------------------------");

        try {
            WePay wepay = new WePay(
                "M515637",
                "nca3twhvpqveixu2hdeutb6utpiet6k7",
                "http://127.0.0.1:8088",
                "MD5",
                ""
            );

            Map<String, String> orderParams = new HashMap<>();
            orderParams.put("out_trade_no", "order_java_" + System.currentTimeMillis());
            orderParams.put("pay_type", "wxpay");
            orderParams.put("amount", "0.01");
            orderParams.put("subject", "Java SDK 测试订单");

            JsonObject result = wepay.createOrder(orderParams);
            System.out.println("下单结果: " + result.toString());

            if (result.get("code").getAsInt() == 1) {
                System.out.println("✓ 下单成功！");
                System.out.println("  平台订单号: " + result.get("trade_no").getAsString());
                System.out.println("  支付链接: " + result.get("pay_url").getAsString());
            } else {
                System.out.println("✗ 下单失败: " + result.get("msg").getAsString());
            }
        } catch (Exception e) {
            System.out.println("✗ 错误: " + e.getMessage());
            e.printStackTrace();
        }

        // ============ 查询订单 ============

        System.out.println("\n[2] MD5 签名 - 查询订单");
        System.out.println("--------------------------------------------------------------------------------");

        try {
            WePay wepay = new WePay(
                "M515637",
                "nca3twhvpqveixu2hdeutb6utpiet6k7",
                "http://127.0.0.1:8088",
                "MD5",
                ""
            );

            JsonObject queryResult = wepay.queryOrder("order_java_test_123");
            System.out.println("查询结果: " + queryResult.toString());
        } catch (Exception e) {
            System.out.println("✗ 错误: " + e.getMessage());
        }

        // ============ 不同支付方式示例 ============

        System.out.println("\n[3] 不同支付方式示例");
        System.out.println("--------------------------------------------------------------------------------");

        String[] payTypes = {"wxpay", "alipay", "qqpay"};

        try {
            WePay wepay = new WePay(
                "M515637",
                "nca3twhvpqveixu2hdeutb6utpiet6k7",
                "http://127.0.0.1:8088",
                "MD5",
                ""
            );

            for (String payType : payTypes) {
                Map<String, String> params = new HashMap<>();
                params.put("out_trade_no", "order_" + payType + "_" + System.currentTimeMillis());
                params.put("pay_type", payType);
                params.put("amount", "0.01");
                params.put("subject", payType + " 测试订单");

                try {
                    JsonObject result = wepay.createOrder(params);
                    if (result.get("code").getAsInt() == 1) {
                        System.out.println("✓ " + payType + ": 下单成功 (订单号: " + result.get("trade_no").getAsString() + ")");
                    } else {
                        System.out.println("✗ " + payType + ": " + result.get("msg").getAsString());
                    }
                } catch (Exception e) {
                    System.out.println("✗ " + payType + ": 错误 - " + e.getMessage());
                }
            }
        } catch (Exception e) {
            System.out.println("✗ 错误: " + e.getMessage());
        }

        // ============ 异步通知验证示例 ============

        System.out.println("\n[4] 异步通知验证");
        System.out.println("--------------------------------------------------------------------------------");

        try {
            WePay wepay = new WePay(
                "M515637",
                "nca3twhvpqveixu2hdeutb6utpiet6k7",
                "http://127.0.0.1:8088",
                "MD5",
                ""
            );

            Map<String, String> notifyData = new HashMap<>();
            notifyData.put("trade_no", "W20260530152204174895");
            notifyData.put("out_trade_no", "order_test_123");
            notifyData.put("amount", "0.01");
            notifyData.put("status", "1");
            notifyData.put("pay_type", "wxpay");

            String sign = wepay.sign(notifyData);
            System.out.println("生成的签名: " + sign);

            boolean isValid = wepay.verifyNotify(notifyData, sign);
            System.out.println("签名验证结果: " + (isValid ? "✓ 有效" : "✗ 无效"));
        } catch (Exception e) {
            System.out.println("✗ 错误: " + e.getMessage());
        }

        // ============ 错误处理示例 ============

        System.out.println("\n[5] 错误处理示例");
        System.out.println("--------------------------------------------------------------------------------");

        try {
            WePay wepay = new WePay(
                "M515637",
                "nca3twhvpqveixu2hdeutb6utpiet6k7",
                "http://127.0.0.1:8088",
                "MD5",
                ""
            );

            // 缺少必要参数
            Map<String, String> invalidParams = new HashMap<>();
            invalidParams.put("out_trade_no", "test_order");
            // 缺少 pay_type, amount, subject

            JsonObject result = wepay.createOrder(invalidParams);
            System.out.println("结果: " + result.toString());
        } catch (Exception e) {
            System.out.println("✓ 捕获异常: " + e.getMessage());
        }

        // ============ RSA 签名示例 ============

        System.out.println("\n[6] RSA 签名 - 统一下单");
        System.out.println("--------------------------------------------------------------------------------");

        String privateKeyPem = "-----BEGIN PRIVATE KEY-----\n" +
            "MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQDM/CnqlQujQq7y\n" +
            "T5sWEq0KhbAimN8AgvO+J4tbnlQaOzZxRPDmKNEUKJcqgbILuaFCmXe6nSann27s\n" +
            "8WvUsnxXK5FGgNYmVOat/wacIkz5wk/KkJDvK3wuB/a7+jmfc/2dgxoOAc+NT34E\n" +
            "0LvvYaOhLHAl6Nyz6KXnSpdE2SXJKVibEF1O/debGo3bQz+OTJFJM8qqE6DiEiZb\n" +
            "VnUNbeYwq2Fmj/WXVUF5nnzXqVS+is9vtvH2hsRGBA+NhHAeA8WDZrHfh3BCqHaS\n" +
            "L++SaWici87+2a7LfRir+ojmnMs8aqGNIiKXmR+hkCdYp+InLf/pkNX17Cco6UdM\n" +
            "J2q1BZENAgMBAAECggEAG8bo5C+dw9oPrGiypo9R0Qz0JQALqf1Uy7X+maPvGB3h\n" +
            "fwBdV4b87AsjDuDD0HhvXH/A3HIasJi3dpaxasFj/Yj7Fu9y9X9IQhg+nE4+mZKl\n" +
            "7thft3UwTumH2wmpoMyeN6eyEmdW6Xp15G+no+TagEbuDIkNTTjPsHOoY208hFFe\n" +
            "p30Wu4JNA8hbk+aKl3mx8tL/iW1T0zLzi7vvXwBd/I7E/vRIZ98GVPQWbqJsy844\n" +
            "aa4rM9bqnR1nxGmsBC3KeY4sOI7o0Z/jFhjDQQCAFp9OWBS1BQPVQjvjXWv9ky4v\n" +
            "UofEVrFH1PNP5ec7M/Zv7GkK3BZuH1+8IFER3dNcSQKBgQDo2THUUPCJx4/aRSjD\n" +
            "ZClFjsnZJB7UvhEOUwur9vCTnARycEqJPzjJPKr8jMxdYn04DXmAQvQhWQobUA8S\n" +
            "9SDBZIAi20ZIMSmTe4YC2cc2YK3d+wJArigZMmV9BhES7D8Uces3gxMM2y/WDa3j\n" +
            "WXWkoAslfkmNQydfqmujcW9gNQKBgQDhXb+KVlMbP3489VIgnIqkAFHDGbtAr1uF\n" +
            "vYRbIaNz+mxqENzaMJ79X5h6VzDH7vsSQH051e4wvlIAHy2WlhCqXBhE3lRjI807\n" +
            "KwUBiJDA73VcOxfRdVYHHoPxObPpSMafaX1yIgwM7k+hpb9EBHNBfBlfomXhJ59m\n" +
            "TPiW7q+4eQKBgQCeLS1UdcdxUUe/lsuiMCB5SA6Gm6r2Cke7215Ka23yWEING4sG\n" +
            "wRPqYHQnK96IcadutHidUN5W6Q2ckD4tOqgNuB/zjdGoqPz9WyQmO5rArdxut11I\n" +
            "YwaKV1nqHHzsxd/0G48WHsyKJzvPxWsizlrEgpQP3EJK3BubOUH1vdFTIQKBgH8V\n" +
            "Ollr7FlFKI5/V9yD6bopY/G8pNcJC3cTM3ugMGfKIzB8ac2v9TeznGwAlsVngbT9\n" +
            "IKBofnSGHf9rlW2BGcy3Ogg7xyJQof5nd98xf08MuQVVXU0D+YryLjzs6QL3wulJ\n" +
            "ty+Q+3KfP9BLgtt8FvIqZLSFAyZADabGaLfTyMshAoGAFQSbx091g5+xX7GP5DMo\n" +
            "nv4BRVtXZ5d7nl2WaI1r5t7+mhFZyC5H7m1fe5A1kxfL1A38xs5iw8yBKDl8cDIT\n" +
            "eHvB50Lgh8MINsSh+c+InpX05WXh5jlKxcvbuFgGWxcz8QYoiOWIfcCRf6az+lfe\n" +
            "qZZfIrQB3NrrzdAZ5mZhOhw=\n" +
            "-----END PRIVATE KEY-----";

        try {
            WePay wePayRsa = new WePay(
                "M515637",
                "nca3twhvpqveixu2hdeutb6utpiet6k7",
                "http://127.0.0.1:8088",
                "RSA",
                privateKeyPem
            );

            Map<String, String> orderParamsRsa = new HashMap<>();
            orderParamsRsa.put("out_trade_no", "order_rsa_java_" + System.currentTimeMillis());
            orderParamsRsa.put("pay_type", "alipay");
            orderParamsRsa.put("amount", "0.02");
            orderParamsRsa.put("subject", "Java SDK RSA 签名测试订单");

            JsonObject result = wePayRsa.createOrder(orderParamsRsa);
            System.out.println("下单结果: " + result.toString());

            if (result.get("code").getAsInt() == 1) {
                System.out.println("✓ RSA 签名下单成功！");
                System.out.println("  平台订单号: " + result.get("trade_no").getAsString());
            } else {
                System.out.println("✗ 下单失败: " + result.get("msg").getAsString());
            }
        } catch (Exception e) {
            System.out.println("✗ 错误: " + e.getMessage());
            e.printStackTrace();
        }

        System.out.println("\n================================================================================");
        System.out.println("示例完成！");
        System.out.println("================================================================================");
    }
}
