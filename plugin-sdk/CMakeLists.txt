cmake_minimum_required(VERSION 3.15)

# 插件名称 — 修改为你的插件名（必须与目录名、plugin.json 的 name 一致）
set(PLUGIN_NAME "demo" CACHE STRING "Plugin name")
project(${PLUGIN_NAME})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Drogon 头文件路径（安装了 Drogon 则自动找到） ───────────────
find_package(Drogon QUIET)
if(NOT Drogon_FOUND)
    # 未安装 Drogon 时手动指定头文件目录
    set(DROGON_INCLUDE_DIR "" CACHE PATH "Drogon include directory (if not installed)")
    if(NOT DROGON_INCLUDE_DIR)
        message(FATAL_ERROR
            "\n未找到 Drogon！请先安装 Drogon 或手动指定:"
            "\n  cmake .. -DDROGON_INCLUDE_DIR=/path/to/drogon/include"
        )
    endif()
endif()

# ── 插件共享库 ──────────────────────────────────────────
add_library(${PLUGIN_NAME} SHARED
    ${PLUGIN_NAME}_plugin.cpp
)

target_include_directories(${PLUGIN_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}       # IPlugin.h 所在目录
    $<$<BOOL:${DROGON_INCLUDE_DIR}>:${DROGON_INCLUDE_DIR}>
)

if(Drogon_FOUND)
    target_link_libraries(${PLUGIN_NAME} PRIVATE Drogon::Drogon)
endif()

# Windows: 去掉 lib 前缀，输出 demo.dll、Linux: demo.so
set_target_properties(${PLUGIN_NAME} PROPERTIES PREFIX "")

# ── 打包为 zip 目标 ─────────────────────────────────────────
if(UNIX)
    add_custom_target(package
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PLUGIN_NAME}
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PLUGIN_NAME}> ${PLUGIN_NAME}/
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/plugin.json ${PLUGIN_NAME}/
        COMMAND zip -r ${PLUGIN_NAME}.zip ${PLUGIN_NAME}/
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${PLUGIN_NAME}/
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "打包插件为 ${PLUGIN_NAME}.zip"
    )
else()
    add_custom_target(package
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PLUGIN_NAME}
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PLUGIN_NAME}> ${PLUGIN_NAME}/
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/plugin.json ${PLUGIN_NAME}/
        COMMAND tar -acf ${PLUGIN_NAME}.zip ${PLUGIN_NAME}/
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${PLUGIN_NAME}/
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "打包插件为 ${PLUGIN_NAME}.zip"
    )
endif()
