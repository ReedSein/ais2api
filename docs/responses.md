# OpenAI Responses 兼容接口

使用 `POST /v1/responses`，鉴权、模型 ID、账号调度和代理配置与 Chat Completions 相同。客户端 base URL 为 `http://127.0.0.1:18451/v1`。本项目实现无状态兼容子集；请求和事件格式参考 [OpenAI Responses API](https://developers.openai.com/api/reference/python/resources/responses/methods/create) 与 [流式事件](https://developers.openai.com/api/reference/resources/responses/streaming-events)。

## 调用

```sh
curl http://127.0.0.1:18451/v1/responses \
  -H "Authorization: Bearer YOUR_KEY" -H "Content-Type: application/json" \
  -d '{"model":"ACTUAL_MODEL_ID","instructions":"请简洁回答","input":"你好","store":false}'
```

Python OpenAI SDK 示例（SDK 是客户端依赖，不是后端运行依赖）：

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

`ACTUAL_MODEL_ID` 来自 `/v1/models`。Responses 不改变上游模型的权限和能力。

## 完整历史与函数调用

每次提交完整 `input`，历史消息格式为 `{"role":"user","content":"..."}` 或带 `type: "message"` 的对象。支持 `input_text`、`output_text` 内容数组，图片为 `{"type":"input_image","image_url":"data:image/png;base64,..."}` 或公开 HTTPS 地址。文件 ID、音频和文件输入暂不支持。

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

返回的 `output` 可以加入下一次 `input`。对 `type: "function_call"` 的项执行本地工具，再附加 `{"type":"function_call_output","call_id":"原样保留的 call_id","output":"结果字符串"}`。保留原始用户输入及所有需要的历史。`call_id` 可能是含思考签名的 `call_gemini_...`，应完整保留；不要改为输出项的 `id`。函数参数是 JSON 字符串，工具结果也使用字符串。真实工具执行由客户端负责。

## 参数和边界

- 支持 `instructions`、`stream`、`max_output_tokens`、`temperature`、`top_p`、`reasoning.effort`、函数工具与 `tool_choice: "auto" / "none"`，以及 `text.format` 的 text、json_object 和非 strict json_schema。参数仍受现有 [协议合同](protocol.md) 的上游限制约束。
- 默认不保存响应；可省略 `store` 或传 `false`。`store: true`、非空 `previous_response_id` / `conversation`、`background: true`、输入项引用、自动截断、内置工具、strict schema、强制函数调用、`parallel_tool_calls: false` 和指定推理摘要均明确拒绝。没有响应查询、删除、取消或 WebSocket 接口；HTTP 断连会取消当前生成。
- JSON 输出使用 `object: "response"`、`output`、`status` 和 `usage`。文本位于 `output[].content[]` 的 `output_text` 项；SDK 的 `output_text` 为客户端便捷属性。usage 使用 `input_tokens`、`output_tokens`、`total_tokens` 和缓存/推理 token 明细。
- 流式响应使用命名 SSE，`event` 名称等于 JSON 的 `type`，所有事件带递增 `sequence_number`。正常结束为 `response.completed`，token 上限或过滤为 `response.incomplete`，已发送内容后发生错误为 `response.failed`；不发送 `[DONE]`。思考文本和可见文本分别发送，函数参数在上游完整返回时发送。
- 请求、媒体、输出聚合和流式状态遵守资源限额；超限会报错。兼容验证使用合成生成源和本机真实 HTTP，不代表已验证真实账号的 Responses 调用。
