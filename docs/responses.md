# OpenAI Responses 接口

`POST /v1/responses` 的鉴权、模型 id、账号调度和代理配置都与 Chat Completions 相同，客户端 Base URL 为 `http://127.0.0.1:18451/v1`。本项目实现的是**无状态**兼容子集；请求和事件格式参考 [OpenAI Responses API](https://developers.openai.com/api/reference/python/resources/responses/methods/create) 与 [流式事件](https://developers.openai.com/api/reference/resources/responses/streaming-events)。

## 调用示例

```sh
curl http://127.0.0.1:18451/v1/responses \
  -H "Authorization: Bearer YOUR_KEY" -H "Content-Type: application/json" \
  -d '{"model":"ACTUAL_MODEL_ID","instructions":"请简洁回答","input":"你好","store":false}'
```

Python OpenAI SDK（SDK 只是客户端依赖）：

```python
from openai import OpenAI

client = OpenAI(base_url="http://127.0.0.1:18451/v1", api_key="YOUR_KEY")
response = client.responses.create(
    model="ACTUAL_MODEL_ID", input="你好", store=False,
)
print(response.output_text)

for event in client.responses.create(
    model="ACTUAL_MODEL_ID", input="你好", store=False, stream=True,
):
    if event.type == "response.output_text.delta":
        print(event.delta, end="", flush=True)
```

`ACTUAL_MODEL_ID` 来自 `/v1/models`。Responses 不会改变上游模型的权限和能力。

## 完整历史与函数调用

服务不保存对话，每次都要提交完整的 `input`：

- 历史消息格式为 `{"role":"user","content":"..."}`，或带 `type: "message"` 的对象。
- 内容数组支持 `input_text` 与 `output_text`。
- 图片为 `{"type":"input_image","image_url":"data:image/png;base64,..."}` 或公开的 HTTPS 地址。
- 文件 ID、音频和文件输入暂不支持。

函数声明使用 Responses 的扁平格式：

```json
{
  "model": "ACTUAL_MODEL_ID",
  "input": "查询天气",
  "store": false,
  "tools": [{
    "type": "function",
    "name": "weather",
    "description": "查询城市天气",
    "parameters": {"type": "object", "properties": {"city": {"type": "string"}}},
    "strict": false
  }],
  "tool_choice": "auto"
}
```

多轮调用流程：

1. 把返回的 `output` 加入下一次的 `input`，同时保留原始用户输入和所需的历史。
2. 对 `type: "function_call"` 的项在本地执行工具。
3. 追加 `{"type":"function_call_output","call_id":"原样保留的 call_id","output":"结果字符串"}`。

`call_id` 可能是含思考签名的 `call_gemini_...`，必须完整保留，不要改用输出项的 `id`。函数参数是 JSON 字符串，工具结果也使用字符串。工具的实际执行由客户端负责。

## 支持的参数

- `instructions`、`stream`、`max_output_tokens`、`temperature`、`top_p`、`reasoning.effort`。
- 函数工具，以及 `tool_choice: "auto"` / `"none"`。
- `text.format` 的 `text`、`json_object` 和非 strict 的 `json_schema`。
- `web_search` / `web_search_preview` 工具（每个请求一个）：转换为 Chat 的 `web_search_options`，启用 Google 搜索，规则与 [搜索模型](protocol.md#search-models) 相同。`search_context_size`、`user_location` 和 `filters.allowed_domains` 会校验格式，但不生效。是否搜索由模型决定；不返回搜索来源，也不输出 `web_search_call` 项。

## 不支持的参数

以下参数默认明确拒绝：

- `store: true`（可以省略 `store` 或传 `false`）、非空的 `previous_response_id` / `conversation`、`background: true`。
- 输入项引用、自动截断、网页搜索以外的内置工具。
- strict schema、强制函数调用（包括用 `tool_choice` 指定网页搜索）、`parallel_tool_calls: false`、指定推理摘要。

没有响应查询、删除、取消或 WebSocket 接口；HTTP 断开会取消当前生成。

开启全局开关 `ignore_unsupported_parameters` 后，上述存储、续接、后台与控制类参数会被舍弃后继续生成，但仍只使用本次提交的完整历史、同步生成且不保存响应。输入项引用、文件内容和无效请求不会被自动改写。开关默认关闭，支持热更新，完整清单见 [兼容开关](protocol.md#optional-unsupported-parameter-dropping)。

## 输出格式

- **JSON**：`object: "response"`，包含 `output`、`status` 与 `usage`。文本位于 `output[].content[]` 中类型为 `output_text` 的项；SDK 的 `output_text` 是客户端提供的便捷属性。usage 使用 `input_tokens`、`output_tokens`、`total_tokens` 以及缓存/推理 token 明细。
- **流式**：命名 SSE，`event` 名称等于 JSON 的 `type`，所有事件带递增的 `sequence_number`。正常结束为 `response.completed`；达到 token 上限或被过滤为 `response.incomplete`；已经输出内容后发生错误为 `response.failed`。不发送 `[DONE]`。思考文本与可见文本分别发送，函数参数在上游完整返回后发送。
- 请求、媒体、输出聚合和流式状态都受资源限额约束，超限会报错。

## 验证范围

兼容性验证使用合成生成源和本机真实 HTTP，不代表已经在真实账号上验证 Responses 调用。详见 [验证记录](validation.md)。
