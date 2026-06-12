#include <iostream>
#include <iomanip>
#include <chrono>
#include "WePay.h"

int main() {
    std::cout << std::string(80, '=') << std::endl;
    std::cout << "WePay C++ SDK 示例" << std::endl;
    std::cout << std::string(80, '=') << std::endl;

    // ============ MD5 签名示例 ============

    std::cout << "\n[1] MD5 签名 - 统一下单" << std::endl;
    std::cout << std::string(80, '-') << std::endl;

    try {
        WePay wepay(
            "M515637",
            "nca3twhvpqveixu2hdeutb6utpiet6k7",
            "http://127.0.0.1:8088",
            "MD5"
        );

        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());

        std::map<std::string, std::string> orderParams;
        orderParams["out_trade_no"] = "order_cpp_" + std::to_string(ms.count());
        orderParams["pay_type"] = "wxpay";
        orderParams["amount"] = "0.01";
        orderParams["subject"] = "C++ SDK 测试订单";

        auto result = wepay.createOrder(orderParams);
        std::cout << "下单结果: " << std::setw(2) << result << std::endl;

        if (result["code"] == 1) {
            std::cout << "✓ 下单成功！" << std::endl;
            std::cout << "  平台订单号: " << result["trade_no"] << std::endl;
            std::cout << "  支付链接: " << result["pay_url"] << std::endl;
        } else {
            std::cout << "✗ 下单失败: " << result["msg"] << std::endl;
        }
    } catch (const std::exception& e) {
        std::cout << "✗ 错误: " << e.what() << std::endl;
    }

    // ============ 查询订单 ============

    std::cout << "\n[2] MD5 签名 - 查询订单" << std::endl;
    std::cout << std::string(80, '-') << std::endl;

    try {
        WePay wepay(
            "M515637",
            "nca3twhvpqveixu2hdeutb6utpiet6k7",
            "http://127.0.0.1:8088",
            "MD5"
        );

        auto result = wepay.queryOrder("order_cpp_test_123");
        std::cout << "查询结果: " << std::setw(2) << result << std::endl;
    } catch (const std::exception& e) {
        std::cout << "✗ 错误: " << e.what() << std::endl;
    }

    // ============ 不同支付方式示例 ============

    std::cout << "\n[3] 不同支付方式示例" << std::endl;
    std::cout << std::string(80, '-') << std::endl;

    std::vector<std::string> payTypes = {"wxpay", "alipay", "qqpay"};

    try {
        WePay wepay(
            "M515637",
            "nca3twhvpqveixu2hdeutb6utpiet6k7",
            "http://127.0.0.1:8088",
            "MD5"
        );

        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());

        for (const auto& payType : payTypes) {
            std::map<std::string, std::string> params;
            params["out_trade_no"] = "order_" + payType + "_" + std::to_string(ms.count());
            params["pay_type"] = payType;
            params["amount"] = "0.01";
            params["subject"] = payType + " 测试订单";

            try {
                auto result = wepay.createOrder(params);
                if (result["code"] == 1) {
                    std::cout << "✓ " << payType << ": 下单成功 (订单号: " 
                              << result["trade_no"] << ")" << std::endl;
                } else {
                    std::cout << "✗ " << payType << ": " << result["msg"] << std::endl;
                }
            } catch (const std::exception& e) {
                std::cout << "✗ " << payType << ": 错误 - " << e.what() << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cout << "✗ 错误: " << e.what() << std::endl;
    }

    // ============ 异步通知验证示例 ============

    std::cout << "\n[4] 异步通知验证" << std::endl;
    std::cout << std::string(80, '-') << std::endl;

    try {
        WePay wepay(
            "M515637",
            "nca3twhvpqveixu2hdeutb6utpiet6k7",
            "http://127.0.0.1:8088",
            "MD5"
        );

        std::map<std::string, std::string> notifyData;
        notifyData["trade_no"] = "W20260530152204174895";
        notifyData["out_trade_no"] = "order_test_123";
        notifyData["amount"] = "0.01";
        notifyData["status"] = "1";
        notifyData["pay_type"] = "wxpay";

        std::string sign = wepay.sign(notifyData);
        std::cout << "生成的签名: " << sign << std::endl;

        bool isValid = wepay.verifyNotify(notifyData, sign);
        std::cout << "签名验证结果: " << (isValid ? "✓ 有效" : "✗ 无效") << std::endl;
    } catch (const std::exception& e) {
        std::cout << "✗ 错误: " << e.what() << std::endl;
    }

    std::cout << "\n" << std::string(80, '=') << std::endl;
    std::cout << "示例完成！" << std::endl;
    std::cout << std::string(80, '=') << std::endl;

    return 0;
}
