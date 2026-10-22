# Google Search 与内置工具转换修复

日期：2026-10-22。范围为 RikkaHub 的 Google generateContent 提供商、SillyTavern 的 Google AI Studio 反向代理，以及 OpenAI 请求中的 `ais2api.gemini` 扩展。

## 原因与修复

- RikkaHub 同时启用自定义函数与内置工具时发送 `toolConfig.includeServerSideToolInvocations:true`，旧白名单拒绝整个请求。现在校验布尔值后接受，内置搜索、URL context 与函数继续转换。该额外调用记录开关没有已验证的私有 wire 映射，写入 `generation_parameters_ignored`，不宣称返回调用记录。
- 嵌套 `search_types/web_search/image_search` 现在递归规范化，拒绝重复拼写。OpenAI 原生扩展也在冲突检查前规范化，保持两种入口一致。
- 旧 `googleSearchRetrieval` 携带动态检索配置会失败。现在验证 `MODE_DYNAMIC/MODE_UNSPECIFIED` 及 [0,1] 阈值，转换为现代 Google Search；动态检索控制记为忽略参数，不承诺阈值生效。
- 内置工具去重、函数声明合为一组；生图模型将分开的网页与图片搜索合为一个配置。与源项目默认识别保持一致，解析别名后的模型 id 包含 `image` 时按生图模型处理，其他模型使用网页搜索。
- 原先沿用捕获模板的顶层位置槽 13，可能缺失或继承旧会话数据。现在每次清除该槽，文本模型存在有效工具时按源项目结构写入 `[[null,null,timezone],null,1]`。生产请求从浏览器 Intl 取得时区，独立 codec 默认 UTC；未声明工具、空工具、NONE 和生图请求不继承位置。

源项目固定参考提交为 [`3e2eff888b21cc403168ceaaa1379a527d8b1a1c`](https://github.com/ReedSein/aistudio-api-termux/tree/3e2eff888b21cc403168ceaaa1379a527d8b1a1c)，主要对照 [chat_service.py](https://github.com/ReedSein/aistudio-api-termux/blob/3e2eff888b21cc403168ceaaa1379a527d8b1a1c/src/aistudio_api/application/chat_service.py) 和 [wire_codec.py](https://github.com/ReedSein/aistudio-api-termux/blob/3e2eff888b21cc403168ceaaa1379a527d8b1a1c/src/aistudio_api/infrastructure/gateway/wire_codec.py)。客户端形状参考 [RikkaHub GoogleProvider.kt](https://github.com/rikkahub/rikkahub/blob/070ff683c0bda1688fdc19c0f41a0666803d213a/ai/src/main/java/me/rerere/ai/provider/providers/google/GoogleProvider.kt) 与 [SillyTavern chat-completions.js](https://github.com/SillyTavern/SillyTavern/blob/06bde939fb1e9c4c8d8641d810f0a916b5bce127/src/endpoints/backends/chat-completions.js)。

## 验证与限制

Windows MSVC Release 构建成功；完整 CTest 通过，164 个用例、2,465 条断言，约 31 秒。`node --check browser/bridge.js` 与 `node --test browser/test_bridge.cjs` 通过，浏览器脚本共 27 项；扩展 schema JSON 解析和 `git diff --check` 通过。

自动回归覆盖四类内置工具的精确 wire、网页/图片搜索合并与去重、客户端混用字段、非法值及别名冲突、捕获模板清理、非 UTC 时区和 OpenAI 扩展。HTTP 使用真实回环服务及执行 `encode_request` 的合成后端，覆盖普通 JSON 与 SSE；浏览器脚本验证 capability 提供当前时区。

本轮检查的项目本地 runtime 没有已导入账号，未完成真实 Google 搜索、客户端应用 UI 或来源引用验收。离线转换成功不代表具体账号和模型具备所有工具权限，也不证明每个请求都会执行搜索。源项目同样未解析搜索引用信息，本次未猜测私有响应槽位，`groundingMetadata`、来源引用及搜索入口元数据仍未实现。

RikkaHub 选择 Google 提供商，Base URL 为 `http://HOST:18451/v1beta`；SillyTavern 选择 Google AI Studio，反向代理为 `http://HOST:18451`，密钥使用网关 API Key。显式 NONE 继续关闭全部工具；ANY/VALIDATED 强制调用、OpenAI Responses `web_search`、Google Interactions 不在本次支持范围。

Google Search 配置启用工具，是否搜索由模型决定，参见 [Google 官方说明](https://ai.google.dev/gemini-api/docs/google-search/)。此公开 API 文档不能用作私有 wire 槽位的证据。
