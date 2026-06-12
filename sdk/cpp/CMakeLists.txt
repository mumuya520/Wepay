cmake_minimum_required(VERSION 3.10)
project(WePay_CPP_SDK)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找必要的库
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

# 如果使用 nlohmann/json，需要下载或安装
# 可以从 https://github.com/nlohmann/json 下载 single_include/nlohmann/json.hpp

# 创建示例可执行文件
add_executable(wepay_example example.cpp)

# 链接库
target_link_libraries(wepay_example 
    PRIVATE 
    OpenSSL::SSL 
    OpenSSL::Crypto 
    CURL::libcurl
)

# 包含目录
target_include_directories(wepay_example 
    PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OPENSSL_INCLUDE_DIR}
    ${CURL_INCLUDE_DIR}
)

# 编译选项
if(MSVC)
    target_compile_options(wepay_example PRIVATE /W4)
else()
    target_compile_options(wepay_example PRIVATE -Wall -Wextra -Wpedantic)
endif()
