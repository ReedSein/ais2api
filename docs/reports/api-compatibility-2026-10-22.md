# API 兼容审查（2026-10-22）

本程序提供 Gemini、OpenAI Chat Completions 与 Responses 的兼容子集，不能承诺完整替代所有厂商 API。此次审查覆盖生成请求归一化、schema 编码、函数历史与签名、聚合响应、流式事件、模型列表/详情、鉴权、请求媒体类型和管理入口。

## Cherry Studio 报错与修复

`schema: unsupported field additionalProperties` 来自本地 `encode_schema` 白名单，发生在向 Google 发起请求之前。原实现还拒绝常见的 title/default 和本地引用，导致 MCP 工具与生成式 schema 无法通用。不是 API Key 或模型名称导致的错误。

三种 API 共用新的 schema 适配层，工具参数、Chat response_format、Responses text.format、Gemini responseSchema / responseJsonSchema 均适用：

- 保留原有递归属性、数组、必填字段、可空类型、约束与联合编码。
- additionalProperties 的布尔值和 schema、title、default 及数字/布尔 enum 转成 description 中的模型提示；字符串 const 转成原生单值 enum。
- 展开本地对象 JSON Pointer 引用，支持 `$defs`、definitions 与转义键；不访问引用 URL。远程、循环、无效或冲突引用明确拒绝。
- 不向私有 Schema 数组注入未经核实的新字段。忽略或降级的关键字记录在 `generation_parameters_ignored`，不写属性名或参数值。
- 展开限制为深度 64、4096 节点与累计 8 MiB 复制/描述预算，阻止引用放大内存。

这些是非 strict 兼容处理。网页接口没有核实的 additionalProperties 强制约束字段，提示不能保证模型遵守；调用方必须验证工具参数和 JSON 输出。strict=true 默认返回明确错误；可显式开启下述开关舍弃 strict 要求。

## 后续增加的参数舍弃开关

新增 `ignore_unsupported_parameters`，默认 false。管理界面「配置 → API 兼容性」可打开；也可编辑 config.json 后执行 `ais2api config reload`，或使用环境变量 `AIS2API_IGNORE_UNSUPPORTED_PARAMETERS=true`。该配置支持热更新。

开启后，已识别的不支持可选参数在归一化前移除，包括 strict、强制工具调用、多候选/单工具限制、存储/后台/续接控制、模型不兼容工具以及部分 JSON Schema 约束；不能表示的有效可选 schema 可整体删除。记录被舍弃字段名，保留内容、历史、身份验证、冲突和资源上限。完整范围及整体选项的校验边界见 [协议合同](../protocol.md#optional-unsupported-parameter-dropping)。没有把任意上游 400 当作可以丢参重试的依据。

开关新增后，MSVC Release（含 WebUI）构建、前端类型检查和打包通过；全量 CTest 通过 **207 个测试、3241 项断言**（33.45 秒）。回归覆盖默认关闭、布尔配置校验、JSON/环境变量往返及优先级、三种 API 的普通/SSE 热更新、舍弃后的实际 wire 与 malformed 输入继续拒绝。本轮仍使用合成后端，没有真实模型或 Cherry Studio GUI 端到端验收。

CLI/IPC/HTTP smoke 另通过 34 项检查并确认临时服务停止、测试目录清理完成。

另修复 Bearer scheme / JSON media type 的大小写兼容性，统一数据、管理与 Cookie 导入入口；保留精确边界检查，拒绝 BearerX 与 application/jsonp。Chat 现在与 Responses 一致拒绝无法保证的 parallel_tool_calls=false，避免接受后静默忽略。

## 对外接口与边界

| 接口 | 支持范围 | 主要限制 |
|---|---|---|
| `/v1/models` 与模型详情 | OpenAI 模型对象与列表 | 依赖发现、缓存或显式配置，不能据此保证账号生成权限 |
| `/v1beta/models` 与模型详情 | Gemini 模型信息，displayName 回退 | 无 countTokens 等其他模型操作 |
| Gemini generateContent / streamGenerateContent | 完整历史、文本/内联媒体、函数、JSON、SSE、原生工具选项 | 无 Files/fileData、多候选、强制/validated 工具调用；部分参数降级或忽略 |
| `/v1/chat/completions` | 文本/图片、函数历史与结果、非 strict JSON、SSE/usage | 无旧版 completions、严格解码、n>1、单调用保证 |
| `/v1/responses` | 无状态完整历史、函数、图片、非 strict JSON、命名 SSE | 无服务端存储、previous_response_id、后台任务、查询/删除、WebSocket |
| `/api/admin/v1/` | 独立管理密钥保护，管理 OpenAPI 合同 | 不是模型厂商协议；空管理密钥禁用 |
| Cookie 导入入口 | 独立导入/管理鉴权与来源校验 | 需身份验证，不能作为通用账号上传接口 |
| `/health`、`/ready` | 进程与本地前提检查 | 不证明 Google 账号和生成可用 |

未提供 Anthropic Messages、embeddings、独立图像/音频/视频 API、Files 或批处理。未知参数继续明确失败，避免把静默丢参当作兼容。thinkingBudget、seed 和搜索额外记录等现有边界见 [协议合同](../protocol.md)。

## 验证范围

回归新增 schema 嵌套、映射、元数据、类型推断、引用转义、循环/缺失/远程/冲突引用与字节放大测试；并使用真实 loopback HTTP 和合成后端，交叉验证 Gemini、Chat、Responses 的普通及 SSE 请求均生成相同的工具/响应 schema wire。HTTP 测试覆盖鉴权 scheme 和 Content-Type 大小写、空格、参数及非法前缀。

Windows MSVC Release 构建通过；协议/Responses 专项通过 57 个测试、1377 项断言；全量 CTest 在正常用户环境通过 190 个测试、2971 项断言（32.99 秒）。初次受限环境集成运行停滞后终止，该次不计为通过。本次没有操作 Cherry Studio GUI 或使用真实 Google 账号生成，也没有重跑 Linux/Termux；本地协议和合成 HTTP 通过不能替代客户端及真实上游验收。

参考：[Google 结构化输出](https://ai.google.dev/gemini-api/docs/structured-output)、[OpenAI 函数调用](https://developers.openai.com/api/docs/guides/function-calling)。网页私有 wire 的已核实字段边界以本项目协议合同为准，不能直接套用公共 REST API 字段。
