#!/bin/bash
# MinIO 生命周期策略配置脚本

set -e

# 配置变量
MINIO_ENDPOINT="http://minio.example.com:9000"
MINIO_ACCESS_KEY="AKIAIOSFODNN7EXAMPLE"
MINIO_SECRET_KEY="wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"
MINIO_ALIAS="wepay-minio"
BUCKET_NAME="wepay-ocr-images"

echo "=== MinIO 生命周期策略配置 ==="

# 1. 配置 MinIO 别名
echo "1. 配置 MinIO 别名..."
mc alias set ${MINIO_ALIAS} ${MINIO_ENDPOINT} ${MINIO_ACCESS_KEY} ${MINIO_SECRET_KEY}

# 2. 创建存储桶（如果不存在）
echo "2. 创建存储桶..."
mc mb ${MINIO_ALIAS}/${BUCKET_NAME} --ignore-existing

# 3. 设置版本控制（可选，用于数据保护）
echo "3. 启用版本控制..."
mc version enable ${MINIO_ALIAS}/${BUCKET_NAME}

# 4. 应用生命周期策略
echo "4. 应用生命周期策略..."

# 策略1: OCR原图 - 90天归档到GLACIER，180天删除
mc ilm add --expiry-days 180 --transition-days 90 --storage-class GLACIER \
    --prefix "ocr/" \
    ${MINIO_ALIAS}/${BUCKET_NAME}

# 策略2: OCR缩略图 - 90天删除
mc ilm add --expiry-days 90 \
    --prefix "ocr/" \
    --tags "type=thumbnail" \
    ${MINIO_ALIAS}/${BUCKET_NAME}

# 策略3: 临时文件 - 7天删除
mc ilm add --expiry-days 7 \
    --prefix "temp/" \
    ${MINIO_ALIAS}/${BUCKET_NAME}

# 5. 查看生命周期策略
echo "5. 查看当前生命周期策略..."
mc ilm ls ${MINIO_ALIAS}/${BUCKET_NAME}

# 6. 导出生命周期策略到JSON文件
echo "6. 导出生命周期策略..."
mc ilm export ${MINIO_ALIAS}/${BUCKET_NAME} > minio-lifecycle-export.json

echo "=== 配置完成 ==="
echo ""
echo "生命周期策略说明："
echo "  - OCR原图: 90天后归档到GLACIER，180天后删除"
echo "  - OCR缩略图: 90天后删除"
echo "  - 临时文件: 7天后删除"
echo ""
echo "查看策略: mc ilm ls ${MINIO_ALIAS}/${BUCKET_NAME}"
echo "删除策略: mc ilm rm --id <rule-id> ${MINIO_ALIAS}/${BUCKET_NAME}"
