#pragma once
#include <drogon/HttpController.h>
#include <string>
#include <vector>
#include <memory>
#include "OcrResultCache.h"

namespace wepay {
namespace v3 {

// OCR识别结果
struct OcrResult {
    bool success = false;
    double amount = 0.0;
    std::string payType;
    std::string transactionId;
    std::string errorMsg;
};

// OCR服务
class OcrService {
public:
    OcrService();
    explicit OcrService(std::shared_ptr<OcrResultCache> cache);

    // 识别支付截图（带缓存）
    OcrResult recognizePaymentScreenshot(const std::string& imagePath);

    // 上传截图到MinIO
    std::string uploadScreenshot(const std::string& orderId,
                                const std::vector<unsigned char>& imageData);

    // 生成缩略图
    std::vector<unsigned char> generateThumbnail(const std::vector<unsigned char>& imageData,
                                                 int maxWidth = 300,
                                                 int maxHeight = 300);

    // 设置缓存
    void setCache(std::shared_ptr<OcrResultCache> cache);

private:
    std::shared_ptr<OcrResultCache> cache_;

    // 使用Tesseract OCR识别
    OcrResult recognizeWithTesseract(const std::string& imagePath);

    // 使用PaddleOCR识别
    OcrResult recognizeWithPaddleOCR(const std::string& imagePath);

    // 提取金额
    double extractAmount(const std::string& text);

    // 提取支付类型
    std::string extractPayType(const std::string& text);
};

// OCR上传接口控制器
class OcrController : public drogon::HttpController<OcrController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(OcrController::handleUpload,
                  "/api/wepay/v3/ocr", drogon::Post);
    METHOD_LIST_END

    OcrController();

    void handleUpload(const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback);

private:
    std::shared_ptr<OcrService> ocrService_;

    drogon::HttpResponsePtr buildResponse(int code, const std::string& message,
                                         const nlohmann::json& data = {});
};

} // namespace v3
} // namespace wepay
