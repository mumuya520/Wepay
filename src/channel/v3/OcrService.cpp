#include "OcrService.h"
#include "WepayV3Config.h"
#ifdef WEPAY_HAS_TESSERACT
#include <tesseract/baseapi.h>
#include <leptonica/allheaders.h>
#endif
#ifdef WEPAY_HAS_MINIO
#include <miniocpp/client.h>
#include <sstream>
#endif
#include <regex>
#include <fstream>

namespace wepay {
namespace v3 {

OcrService::OcrService() : cache_(nullptr) {}

OcrService::OcrService(std::shared_ptr<OcrResultCache> cache) : cache_(cache) {}

void OcrService::setCache(std::shared_ptr<OcrResultCache> cache) {
    cache_ = cache;
}

OcrResult OcrService::recognizePaymentScreenshot(const std::string& imagePath) {
    // 1. 计算图片哈希
    std::string imageHash;
    try {
        imageHash = OcrResultCache::calculateImageHash(imagePath);
    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to calculate image hash: " << e.what();
        imageHash = "";
    }

    // 2. 检查缓存
    if (cache_ && !imageHash.empty()) {
        auto cachedResult = cache_->getCachedResult(imageHash);
        if (cachedResult) {
            LOG_INFO << "OCR cache hit: imageHash=" << imageHash;

            OcrResult result;
            result.success = cachedResult->success;
            result.amount = std::stod(cachedResult->amount);
            result.payType = cachedResult->payType;
            result.transactionId = cachedResult->transactionId;
            result.errorMsg = cachedResult->errorMessage;

            return result;
        }
    }

    // 3. 执行OCR识别
    auto result = recognizeWithTesseract(imagePath);

    if (!result.success) {
        result = recognizeWithPaddleOCR(imagePath);
    }

    // 4. 缓存结果
    if (cache_ && !imageHash.empty()) {
        OcrResultCache::OcrResult cacheResult;
        cacheResult.imageHash = imageHash;
        cacheResult.amount = std::to_string(result.amount);
        cacheResult.payType = result.payType;
        cacheResult.orderId = "";
        cacheResult.transactionId = result.transactionId;
        cacheResult.recognizeTime = std::time(nullptr);
        cacheResult.confidence = result.success ? 0.95 : 0.0;
        cacheResult.success = result.success;
        cacheResult.errorMessage = result.errorMsg;

        cache_->cacheResult(imageHash, cacheResult, 86400); // 缓存24小时
    }

    return result;
}

OcrResult OcrService::recognizeWithTesseract(const std::string& imagePath) {
    OcrResult result;
#ifdef WEPAY_HAS_TESSERACT
    try {
        tesseract::TessBaseAPI ocr;

        // 初始化Tesseract（使用中文简体）
        if (ocr.Init(nullptr, "chi_sim")) {
            result.errorMsg = "Failed to initialize Tesseract";
            return result;
        }

        // 打开图片
        Pix* image = pixRead(imagePath.c_str());
        if (!image) {
            result.errorMsg = "Failed to read image";
            return result;
        }

        ocr.SetImage(image);

        // 执行OCR识别
        char* text = ocr.GetUTF8Text();
        std::string recognizedText(text);

        delete[] text;
        pixDestroy(&image);
        ocr.End();

        // 提取金额和支付类型
        result.amount = extractAmount(recognizedText);
        result.payType = extractPayType(recognizedText);

        if (result.amount > 0) {
            result.success = true;
        } else {
            result.errorMsg = "Failed to extract amount";
        }

    } catch (const std::exception& e) {
        result.errorMsg = std::string("OCR error: ") + e.what();
    }
#else
    (void)imagePath;
    result.errorMsg = "Tesseract not available on this platform";
#endif
    return result;
}

OcrResult OcrService::recognizeWithPaddleOCR(const std::string& imagePath) {
    OcrResult result;

    // TODO: 集成PaddleOCR
    // 这里可以调用PaddleOCR的C++ API或通过HTTP调用Python服务

    result.errorMsg = "PaddleOCR not implemented";
    return result;
}

double OcrService::extractAmount(const std::string& text) {
    // 匹配金额模式：¥123.45 或 123.45元
    std::regex amountPattern(R"(¥?\s*(\d+\.?\d*)\s*元?)");
    std::smatch match;

    if (std::regex_search(text, match, amountPattern)) {
        try {
            return std::stod(match[1].str());
        } catch (...) {
            return 0.0;
        }
    }

    return 0.0;
}

std::string OcrService::extractPayType(const std::string& text) {
    if (text.find("支付宝") != std::string::npos ||
        text.find("alipay") != std::string::npos) {
        return "ALIPAY";
    }

    if (text.find("微信") != std::string::npos ||
        text.find("wechat") != std::string::npos ||
        text.find("WeChat") != std::string::npos) {
        return "WECHAT";
    }

    return "UNKNOWN";
}

std::string OcrService::uploadScreenshot(const std::string& orderId,
                                        const std::vector<unsigned char>& imageData) {
#ifdef WEPAY_HAS_MINIO
    try {
        auto& config = WepayV3Config::getInstance();

        // 初始化MinIO客户端
        minio::s3::BaseUrl baseUrl(config.minio.endpoint);
        minio::creds::StaticProvider provider(config.minio.accessKey,
                                             config.minio.secretKey);
        minio::s3::Client client(baseUrl, &provider);

        // 生成文件路径：ocr/{date}/{orderId}.jpg
        time_t now = time(nullptr);
        struct tm* timeinfo = localtime(&now);
        char dateStr[32];
        strftime(dateStr, sizeof(dateStr), "%Y%m%d", timeinfo);

        std::string objectName = "ocr/" + std::string(dateStr) + "/" + orderId + ".jpg";

        // 上传原图
        std::stringstream ss;
        ss.write(reinterpret_cast<const char*>(imageData.data()), imageData.size());

        minio::s3::PutObjectArgs args;
        args.bucket = config.minio.bucket;
        args.object = objectName;
        args.stream = &ss;
        args.object_size = (long)imageData.size();
        args.content_type = "image/jpeg";

        auto response = client.PutObject(args);

        if (response) {
            auto thumbnail = generateThumbnail(imageData);
            std::string thumbName = "ocr/" + std::string(dateStr) + "/" + orderId + "_thumb.jpg";

            std::stringstream thumbSs;
            thumbSs.write(reinterpret_cast<const char*>(thumbnail.data()), thumbnail.size());

            args.object = thumbName;
            args.stream = &thumbSs;
            args.object_size = (long)thumbnail.size();
            client.PutObject(args);

            return config.minio.endpoint + "/" + config.minio.bucket + "/" + objectName;
        }

    } catch (const std::exception& e) {
        LOG_ERROR << "Failed to upload screenshot: " << e.what();
    }
#else
    (void)orderId;
    (void)imageData;
    LOG_WARN << "MinIO not available, screenshot upload skipped";
#endif
    return "";
}

std::vector<unsigned char> OcrService::generateThumbnail(
    const std::vector<unsigned char>& imageData,
    int maxWidth,
    int maxHeight) {

    std::vector<unsigned char> thumbnail;

    try {
        // 使用OpenCV生成缩略图
        // TODO: 实现图片缩放逻辑

        // 简单实现：直接返回原图（实际应该缩放）
        thumbnail = imageData;

    } catch (...) {
        thumbnail = imageData;
    }

    return thumbnail;
}

// OcrController实现
OcrController::OcrController() {
    ocrService_ = std::make_shared<OcrService>();
}

void OcrController::handleUpload(
    const drogon::HttpRequestPtr& req,
    std::function<void(const drogon::HttpResponsePtr&)>&& callback) {

    try {
        // 获取上传的文件
        drogon::MultiPartParser parser;
        if (parser.parse(req) != 0 || parser.getFiles().empty()) {
            callback(buildResponse(400, "No file uploaded"));
            return;
        }

        auto& file = parser.getFiles()[0];
        std::string orderId = req->getParameter("orderId");

        if (orderId.empty()) {
            callback(buildResponse(400, "Missing orderId parameter"));
            return;
        }

        // 读取文件数据
        const auto& fileContent = file.fileContent();
        std::vector<unsigned char> imageData(fileContent.begin(), fileContent.end());

        // 写入临时文件供 OCR 使用
        std::string tempPath = "/tmp/ocr_" + orderId + ".jpg";
        {
            std::ofstream ofs(tempPath, std::ios::binary);
            ofs.write(reinterpret_cast<const char*>(imageData.data()), imageData.size());
        }

        // 上传到MinIO
        std::string imageUrl = ocrService_->uploadScreenshot(orderId, imageData);

        if (imageUrl.empty()) {
            callback(buildResponse(500, "Failed to upload screenshot"));
            return;
        }

        // 执行OCR识别
        auto ocrResult = ocrService_->recognizePaymentScreenshot(tempPath);

        nlohmann::json responseData;
        responseData["imageUrl"] = imageUrl;
        responseData["ocrSuccess"] = ocrResult.success;

        if (ocrResult.success) {
            responseData["amount"] = ocrResult.amount;
            responseData["payType"] = ocrResult.payType;
        } else {
            responseData["errorMsg"] = ocrResult.errorMsg;
        }

        callback(buildResponse(200, "success", responseData));

    } catch (const std::exception& e) {
        callback(buildResponse(500, std::string("Internal error: ") + e.what()));
    }
}

drogon::HttpResponsePtr OcrController::buildResponse(
    int code,
    const std::string& message,
    const nlohmann::json& data) {

    nlohmann::json response;
    response["code"] = code;
    response["message"] = message;

    if (!data.empty()) {
        response["data"] = data;
    }

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setContentTypeString("application/json");
    resp->setBody(response.dump());

    if (code == 200) {
        resp->setStatusCode(drogon::k200OK);
    } else if (code == 400) {
        resp->setStatusCode(drogon::k400BadRequest);
    } else {
        resp->setStatusCode(drogon::k500InternalServerError);
    }

    return resp;
}

} // namespace v3
} // namespace wepay
