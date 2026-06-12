cmake_minimum_required(VERSION 3.15)
project(wepay_v3_plugin)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找依赖库
find_package(Drogon REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(hiredis REQUIRED)
find_package(redis++ REQUIRED)
find_package(RocketMQ REQUIRED)
find_package(Tesseract REQUIRED)
find_package(Leptonica REQUIRED)
find_package(nlohmann_json REQUIRED)

# 源文件
set(SOURCES
    WepayV3Config.cpp
    SecurityValidator.cpp
    DeviceManager.cpp
    HeartbeatController.cpp
    WebSocketController.cpp
    WebSocketConnectionManager.cpp
    OrderService.cpp
    OcrService.cpp
    OcrResultCache.cpp
    CacheOptimizer.cpp
    EmailService.cpp
    RocketMQManager.cpp
    DeviceController.cpp
    SentinelFlowControl.cpp
    GrayscaleManager.cpp
    AlertNotification.cpp
    WepayV3Plugin.cpp
)

# 头文件
set(HEADERS
    WepayV3Config.h
    SecurityValidator.h
    DeviceManager.h
    HeartbeatController.h
    WebSocketController.h
    WebSocketConnectionManager.h
    OrderService.h
    OcrService.h
    OcrResultCache.h
    CacheOptimizer.h
    EmailService.h
    RocketMQManager.h
    DeviceController.h
    SentinelFlowControl.h
    GrayscaleManager.h
    AlertNotification.h
    WepayV3Plugin.h
)

# 创建静态库（方便主程序链接）
add_library(wepay_v3_plugin STATIC ${SOURCES} ${HEADERS})

# 链接库
target_link_libraries(wepay_v3_plugin PUBLIC
    Drogon::Drogon
    yaml-cpp
    OpenSSL::SSL
    OpenSSL::Crypto
    hiredis::hiredis
    redis++::redis++
    rocketmq::rocketmq
    tesseract
    leptonica
    nlohmann_json::nlohmann_json
    pq  # PostgreSQL客户端库
    pqxx  # PostgreSQL C++库
)

# 包含目录
target_include_directories(wepay_v3_plugin PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${YAML_CPP_INCLUDE_DIR}
    ${OPENSSL_INCLUDE_DIR}
    ${TESSERACT_INCLUDE_DIRS}
    ${PostgreSQL_INCLUDE_DIRS}
)

# 安装规则
install(TARGETS wepay_v3_plugin
    ARCHIVE DESTINATION lib
)

install(FILES ${HEADERS}
    DESTINATION include/wepay/v3
)

install(FILES config.yaml
    DESTINATION etc/wepay
)

install(FILES database_postgresql.sql
    DESTINATION share/wepay
)

# 编译选项
target_compile_options(wepay_v3_plugin PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -O2
)

# 如果是Debug模式，添加调试符号
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(wepay_v3_server PRIVATE -g)
endif()

# 打印配置信息
message(STATUS "WePay V3 Plugin Configuration:")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Install Prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  Database: PostgreSQL")
message(STATUS "  Message Queue: RocketMQ")
message(STATUS "  Cache: Redis")
message(STATUS "  Object Storage: MinIO")
