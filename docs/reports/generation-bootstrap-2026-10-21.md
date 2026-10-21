# 真实文本生成与 XHR 初始化修复

日期：2026-10-21。Windows 原生后端、CloakBrowser 146.0.7680.177.4；Debian 13 / WSL 2 同时用于构建验证和访问 Windows 服务。

## 故障原因

用户的 `gemini-3.1-flash-lite` 文本请求在生成初始化阶段返回 `browser_incompatible`，提示无法捕获 GenerateContent 模板及快照。旧桥接只在 fetch 分支保存生成模板，XHR 分支只捕获 ListModels。真实页面使用 XHR 生成，因此模板保持为空。

隔离浏览器检查同一账号时，旧桥接已经观察到快照调用，但没有生成模板；新桥接在账号及编辑框就绪后，捕获到 `templateTransport=xhr`，并将模板里的实际票据与适配器返回值匹配。两项能力均就绪，且没有向上游发送占位生成请求。

## 修复内容

- fetch 与 XHR 都捕获页面实际生成的 JSPB 数组及请求头，保留原模型发现流程。
- 初始化占位生成及保存请求在本地完成；XHR 的响应属性、状态变化、load/loadend、取消及重新打开均有独立回归。用户的真实生成由原生 fetch 执行。
- 支持已知快照调用形式，只选择返回票据与实际模板 `body[4]` 相同的适配器；每个用户请求仍用对应内容 hash 获取新票据。
- 使用透明函数 Proxy，保留构造、原型、静态属性、receiver、参数、原始返回值和异常语义；不可替换导出与 getter 不作为可用钩子。
- 初始化错误区分未捕获生成模板和未验证快照适配器，诊断只含状态、通道和计数。

没有复制原项目实现或测试，没有伪造快照、模型思考签名或生成回复。

## 真实复测

沿用原有 Cookie 与配置，模型为 `gemini-3.1-flash-lite`，temperature=0、max_tokens=1000。

| 请求 | 结果 |
|---|---|
| Windows OpenAI 普通生成：`Reply with exactly: OK` | HTTP 200，正文 `OK`，finish_reason=stop，约 20.11 秒（含冷初始化） |
| Windows OpenAI SSE | HTTP 200，正文拼接为 `OK`，3 个 JSON 事件，存在 usage 及 `[DONE]` |
| Windows Gemini generateContent | HTTP 200，正文 `OK`，存在 usageMetadata |
| WSL 调用 Windows OpenAI 多轮 | HTTP 200，先前 user/assistant 为 OK，新一轮正确返回 `NEXT`，存在 usage |

多次请求使用不同内容并重复获取快照；不是重用占位初始化票据。日志只记录是否符合预期、数量与耗时，不保存 Cookie、密钥、签名或上游正文。模型列表、模型详情与 WSL 侧模型调用继续通过。

同日后续扩展验收曾在模型列表预检返回 `401/session_expired`，独立浏览器连续六次停在 Google 登录页；用户更新 Cookie 后，Flash-Lite 媒体/schema/多轮和 Gemma SSE 真实通过，但函数调用返回 403。上表与后续范围分别记录，详情见 [LLM API 对照报告](llm-api-parity-2026-10-21.md)。修复后的桥接已核对同步到实际 Windows 安装目录及浏览器整合包。

## 自动回归与边界

Windows：105 用例、1,599 条断言；Debian GCC / Clang ASan/UBSan：105 用例、1,591 条断言；网页桥接：24 项测试。Windows 真实浏览器离线 Cookie / 身份回归也通过。

本记录覆盖 Windows 上该模型的基础文本单轮、多轮和 OpenAI SSE，以及 Gemini 普通生成。WSL 的真实生成请求访问的是 Windows 服务；不代表 Debian 原生真实账号、Gemini 原生 SSE、工具、多模态、生图、长期稳定性或三端完整发布验收。
