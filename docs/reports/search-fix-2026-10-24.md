# 搜索 502 修复与搜索模型

日期：2026-10-24。用户反馈开启搜索后请求返回 502。本次对照参考项目 [`aistudio-api-termux`](https://github.com/ReedSein/aistudio-api-termux)，修复响应解析，新增 `*-search` 模型，并补齐 OpenAI 入口的搜索支持。

## 对照结论

请求侧的搜索工具 wire 与参考项目一致，包括网页、图片及合并搜索模板、文本模型的时区槽 13、`NONE` 清空工具，以及签名只使用对话内容。差异集中在下面几处：

| 项目 | 参考项目 | 修复前 | 修复后 |
|---|---|---|---|
| 响应 part 未知槽位 | 跳过 | 抛出 `Unsupported upstream part payload`，整个请求 502 | 跳过，记录槽位编号 |
| 非数组 part、格式异常的非函数载荷 | 跳过 | 502 | 跳过并记录；函数调用格式异常仍报错 |
| 不含正文的数据块中出现 `[整数, "字符串"]` | 不检查 | 可能被误判为上游 RPC 错误 | candidate 容器整体视为载荷 |
| 默认搜索 | `gemini-`/`gemma-`/图片模型不传 tools 时注入 | 从不注入 | `*-search` 模型注入，其他模型保持关闭 |
| OpenAI 搜索 | 无 OpenAI 接口 | Chat 拒绝，Responses 舍弃 `web_search` | `web_search_options` 与 `web_search` 都映射为 Google 搜索 |
| 函数声明位置 | 排在内置工具之后 | 排在最前 | 排在内置工具之后 |
| `googleSearch` 公开选项 | 原样接受 | 400 | `timeRangeFilter`、`excludeDomains` 校验后忽略 |

第一行就是 502 的原因，已经实测确认：启用搜索后，上游响应的 part 会带槽位 25 和 26，很可能是服务端搜索工具的调用记录和返回结果；candidate 还会带槽位 7，很可能是来源信息。本项目原先只要出现白名单以外的 part 槽位就失败，参考项目的 `wire_parser.py` 则对所有未知字段都容错。

## 实现

- **响应解析**：`decode_part` / `decode_chunk` 跳过未知或格式异常的非函数槽位，并通过内部键汇总，例如 `part[9]`、`candidate[9]`、`chunk[8]`。服务每个请求写一条 `upstream_fields_ignored` 日志，只记录位置，不记录内容；该内部键在事件发给客户端前删除。
- **RPC 错误识别**：数据块的首个元素如果是 candidate 容器（每个 candidate 以 null 或 `[parts, role]` 开头），就不再向内查找状态信封。多层包装的真实错误仍能识别。
- **搜索模型**：先解析 `model_aliases`，再去掉 `-search` 后缀（不区分大小写）。上游请求、`model_defaults`、账号调度、配额统计和响应里的 `model` 都使用基础模型。注入规则见 [协议合同](../protocol.md#search-models)。
- **新开关 `search_model_drop_functions`**：默认 true，热更新，用于处理 `*-search` 模型收到自定义函数的情况。true 时舍弃函数及非 NONE 的函数调用控制，保留搜索，并记为忽略参数；false 时保留客户端工具，不加搜索。可在配置文件、`AIS2API_SEARCH_MODEL_DROP_FUNCTIONS` 或管理控制台「配置 → API 兼容性」中设置。
- **模型列表**：数据 API 的 Gemini 与 OpenAI 模型列表及详情，会在每个支持 generateContent 的 Gemini、Gemma 和图片模型后追加 `<id>-search`（排除 embedding 和 TTS），`source` 为 `search_variant`。管理控制台的模型页只显示发现缓存。
- **OpenAI**：Chat 的 `web_search_options` 与 `*-search` 规则相同；Responses 的 `web_search` / `web_search_preview` 转为该参数，位置字段改为 Chat 的嵌套格式。上下文大小、位置和域名过滤都会校验格式，但不生效，记为忽略参数。

## 验证

- Windows MSVC Release 构建成功，Catch2 全量 291 个用例、4,813 条断言通过；CTest 通过。
- `node --test browser/test_bridge.cjs` 46 项通过，WebUI `vue-tsc --noEmit` 与 vitest 26 项通过，`git diff --check` 通过。
- 新增回归覆盖：未知槽位跳过与上报、函数格式错误仍失败、grounding 形态数据块不被误判、四层包装的错误仍可识别、`-search` 注入（文本/图片/别名/defaults/NONE/去重）、开关两种取值、`web_search_options` 及其非法值、Responses 映射、公开搜索选项、模型列表变体以及配置持久化。
- **真实账号验收**（2026-10-24，Windows 本机安装实例，二进制与本次构建 SHA-256 一致，模型 `gemini-3.1-flash-lite`）：
  - 下列 8 个请求全部返回 200，每次约 2–11 秒：Gemini 原生 `googleSearch`（普通与 SSE）、`-search` 模型不传 tools、`-search` 模型带自定义函数（日志记录舍弃 `tools.functionDeclarations`）、Chat 的 `web_search_options`、Chat 的 `-search` 模型、Responses 的 `web_search`。
  - 带搜索的回答都给出了训练数据之后才有的信息（Rust 1.99.1，2026-10-15 发布）。不带后缀的 `gemini-3.1-flash-lite` 不传 tools 时回答旧版本 1.85.0，确认默认不搜索。
  - 每个搜索请求的 `upstream_fields_ignored` 日志都记录了 `part[25]`、`part[26]`，多数还有 `candidate[7]`；普通请求没有产生这条日志。OpenAI 响应里的 `model` 为基础模型名。
  - 实时模型列表共 57 项，其中 20 项为搜索变体；Live、TTS、Lyria、Deep Research 等模型没有变体。
- 有 8 个旧用例原先断言旧行为（`web_search` 被拒绝或舍弃、未知 part 报错），已按新合同更新。

## 限制

- 搜索来源仍未解码，Responses 也不输出 `web_search_call`。后续可以根据 `part[25]`/`part[26]` 和 `candidate[7]` 的结构实现来源引用。
- 真实模型 id 本身以 `-search` 结尾时，会被当作搜索变体处理，无法直接调用。
- 舍弃函数后，历史中的函数调用和结果 part 仍会原样发送；上游是否接受未经验证。
