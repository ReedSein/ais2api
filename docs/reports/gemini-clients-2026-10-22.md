# Gemini 客户端兼容修复（2026-10-22）

本次根据客户端官方源码定位请求合同差异，并以独立合成样例验证修复。没有连接真实 Google 账号，也没有在两款客户端应用中完成端到端验收。

## 问题与修复

- RikkaHub 与 SillyTavern 的默认安全设置均包含 `HARM_CATEGORY_CIVIC_INTEGRITY: OFF`，旧编码器因此报错。现在仅对该类别的 `OFF/BLOCK_NONE` 作无操作处理，保留四类已有过滤设置；其他未知或无法实现的启用设置继续报错。
- SillyTavern 发送的 `candidateCount: 1` 曾被拒绝。现在接受单候选，其他数量仍拒绝。
- 两端常见的 `includeThoughts` 曾被拒绝。现在布尔值可用，false 在普通与流式响应中隐藏思考文本，保留签名、工具动作和原始 usage。
- RikkaHub 小写思考等级、SillyTavern 小写输出模态及工具字段 `function_declarations/google_search` 曾被拒绝。现在支持对应大小写及字段别名，保留调用者的函数参数、返回数据和 schema 属性名；重复字段拼写报错。
- RikkaHub 读取模型列表需要 `displayName`，配置字符串模型曾缺失此字段。列表和详情现在回退为模型 id，保留真实显示名及元数据。
- `thinkingBudget` 与 `seed` 没有可靠的私有上游映射。现在校验后接受但忽略，并通过 `generation_parameters_ignored` 日志记录字段名；不记录参数值。预算 0 不关闭思考，这些参数也不会强制 HIGH。显式 `thinkingLevel` 使用已验证映射。

源码依据：[RikkaHub GoogleProvider.kt](https://github.com/rikkahub/rikkahub/blob/070ff683c0bda1688fdc19c0f41a0666803d213a/ai/src/main/java/me/rerere/ai/provider/providers/google/GoogleProvider.kt)、[SillyTavern chat-completions.js](https://github.com/SillyTavern/SillyTavern/blob/06bde939fb1e9c4c8d8641d810f0a916b5bce127/src/endpoints/backends/chat-completions.js)、[SillyTavern constants.js](https://github.com/SillyTavern/SillyTavern/blob/06bde939fb1e9c4c8d8641d810f0a916b5bce127/src/constants.js)。这些链接固定本次核查版本，不代表覆盖未来所有客户端版本。

## 验证结果

Windows MSVC Release 构建成功；完整 CTest 通过：156 个测试用例、2393 个断言，耗时约 31 秒。

新增回归覆盖客户端请求形状、`x-goog-api-key` 与 query key 鉴权、普通 JSON 与 SSE、工具编码、五类安全设置输入、思考显示与签名、大小写和别名冲突、无效控制值、模型列表与详情，并保持 OpenAI 模型列表合同。合成后端实际执行 `encode_request`，避免只验证接收请求而遗漏上游 wire 编码失败。

HTTP 测试使用临时数据、合成账号和回环连接；在允许本机 IPC/网络的测试进程中执行。结果不代表真实账号、浏览器、移动设备、Linux 或 Termux 的端到端验收。

## 连接方式

- RikkaHub：Google 提供商，Base URL 为 `http://HOST:18451/v1beta`，API Key 使用网关密钥。
- SillyTavern：Google AI Studio，反向代理为 `http://HOST:18451`，代理密码使用网关密钥。
- 流式路径为 `/v1beta/models/MODEL:streamGenerateContent?alt=sse`，普通路径为 `:generateContent`；`/v1/models` 保持 OpenAI 列表格式。

`HOST` 应为客户端能够访问的服务地址。完整参数边界见 [协议合同](../protocol.md)。
