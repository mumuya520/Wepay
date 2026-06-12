# 贡献指南

感谢你对 WePay-Cpp 项目的关注！我们欢迎任何形式的贡献。

## 如何贡献

### 报告问题

如果你发现了 bug 或有功能建议：

1. 先在 [Issues](https://github.com/your-repo/wepay-cpp/issues) 中搜索，确认问题未被报告
2. 创建新的 Issue，详细描述问题或建议
3. 提供复现步骤、环境信息、截图等

### 提交代码

1. **Fork 仓库**
   - 点击右上角 Fork 按钮

2. **创建分支**
   ```bash
   git checkout -b feature/your-feature-name
   # 或
   git checkout -b fix/your-bug-fix
   ```

3. **编写代码**
   - 遵循现有代码风格
   - 添加必要的注释
   - 编写测试（如果适用）

4. **提交更改**
   ```bash
   git add .
   git commit -m "feat: 添加新功能描述"
   # 或
   git commit -m "fix: 修复问题描述"
   ```

5. **推送到分支**
   ```bash
   git push origin feature/your-feature-name
   ```

6. **创建 Pull Request**
   - 在 GitHub 上创建 PR
   - 填写 PR 模板
   - 等待代码审查

## 代码规范

### C++ 代码风格

- 使用 C++20 标准
- 遵循 Google C++ Style Guide
- 使用 4 空格缩进
- 函数命名使用大驼峰：`CreateOrder()`
- 变量命名使用小驼峰：`orderId`
- 常量使用全大写下划线：`MAX_RETRY_COUNT`

### 提交信息规范

使用 [Conventional Commits](https://www.conventionalcommits.org/) 规范：

- `feat:` 新功能
- `fix:` 修复 bug
- `docs:` 文档更新
- `style:` 代码格式调整
- `refactor:` 重构
- `test:` 测试相关
- `chore:` 构建/工具相关

示例：
```
feat(gateway): 添加支付宝支付通道支持
fix(order): 修复订单查询时的空指针问题
docs(readme): 更新 Windows 编译说明
```

### 代码审查

所有 PR 都需要经过代码审查：

- 至少需要一位维护者批准
- 通过所有 CI 检查
- 解决所有审查意见

## 开发环境设置

### 1. 克隆仓库

```bash
git clone https://github.com/your-username/wepay-cpp.git
cd wepay-cpp
```

### 2. 安装依赖

参考 [README.md](README.md) 中的环境要求

### 3. 编译项目

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)  # Linux
# 或
cmake --build . --config Release  # Windows
```

### 4. 运行测试

```bash
ctest
```

## 插件开发

如果你想开发支付通道插件，请参考：

- [插件 SDK 文档](plugin-sdk/README.md)
- [插件开发示例](plugin-sdk/插件开发/README.md)

## 文档贡献

文档同样重要！你可以：

- 修正文档错误
- 添加使用示例
- 翻译文档
- 补充 API 说明

## 行为准则

- 尊重所有贡献者
- 保持友好和专业
- 接受建设性批评
- 关注对社区最有利的事情

## 获取帮助

如果你在贡献过程中遇到问题：

- 在 [Discussions](https://github.com/your-repo/wepay-cpp/discussions) 中提问
- 联系维护者
- 查看 [文档](README.md#文档)

## 许可证

通过贡献代码，你同意你的贡献将根据项目的 [MIT 许可证](LICENSE) 进行许可。

---

再次感谢你的贡献！🎉
