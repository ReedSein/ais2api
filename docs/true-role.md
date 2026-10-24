# 真 role 与思考签名

ais2api 通过现有的 Gemini 与 OpenAI 接口提供“真 role”转发和客户端思考签名往返，无需额外开关。基本原则：

- 每次请求由客户端提交完整历史，服务不保存聊天记录。
- 保留每条消息的角色、边界与部件顺序，连续的同角色消息不会被合并。
- 不为手写的 assistant 消息补签名，也不插入分隔消息。

## Gemini 原生接口

`/v1beta/models/MODEL:generateContent` 与 `:streamGenerateContent` 保留每条 content 的角色、消息边界与部件顺序，连续的 user/user、model/model 原样编码。`systemInstruction` 单独编码。未声明 tools 时不会自动启用搜索，模型 id 以 `-search` 结尾时除外（见 [搜索模型](protocol.md#search-models)）。

下一轮请求应把响应候选中**完整的 content** 放回历史，而不是只提取可见文字：

```json
{
  "systemInstruction": {"parts": [{"text": "系统提示"}]},
  "contents": [
    {"role": "user", "parts": [{"text": "第一轮"}]},
    {"role": "model", "parts": [
      {"text": "模型返回的思考", "thought": true},
      {"text": "模型返回的正文"},
      {"text": "", "thoughtSignature": "此处须使用模型原始签名"}
    ]},
    {"role": "user", "parts": [{"text": "继续"}]}
  ],
  "tools": []
}
```

示例中的签名文字只用于说明字段位置，不能作为有效签名发送。

使用规则：

- 签名是不透明的模型输出，应跟随原部件，不要修改、移动或伪造。
- 编辑可见正文时可以保留其余原部件；删除消息时，连同该消息的思考与签名一起删除。
- 手写的 assistant 消息直接使用普通 model 文本即可。
- 签名跨模型复用的效果没有验证。

流式与非流式输出都保留独立的签名部件，以及函数调用、代码执行和执行结果的部件顺序。只有相邻且都不带签名的同类文本增量会合并。格式异常的函数部件返回 502，避免静默丢失上下文；其他未知或异常的部件数据会被跳过并记录位置（见 [协议合同](protocol.md#响应解析)）。

## OpenAI 兼容接口

`/v1/chat/completions` 保留普通消息的边界：

- 开头连续的 system / developer 消息转为独立的系统提示；对话开始后出现的 system / developer 消息在原位置转为 user 文本，不添加角色标签。
- 连续的 tool 结果合并为同一条 user 消息中的多个结果部件。

### 思考与文本签名

assistant 消息可以用 `reasoning_content`、`content` 与 `thought_signature`（输入也接受 `signature`）带回思考、正文和独立的文本签名。响应消息和流式 delta 都提供这些兼容字段。

如果需要保留交错的原生部件，请使用 Gemini 原生接口，或原样回传 `message.ais2api.gemini_content`；兼容字段与原生内容同时提供时会检查一致性。

### 工具调用签名

带签名的工具调用把原始调用 ID、签名和用于区分调用的 nonce 编码在 `call_gemini_...` ID 中。客户端只需原样保存 `tool_calls[].id` 与 `tool_call_id`，服务会从历史中的 ID 恢复签名，无需服务端保存。

- 不要截断、重写或解析这个 ID。
- 原调用没有 ID 时，转回 Gemini 后仍不添加 ID。
- 未编码的旧调用 ID 仍然接受，但服务不会补回旧版本保存在数据库中的签名。

### 客户端行为

参考文档对 SillyTavern / Luker 的读码结论是：Google AI Studio 来源开启 Streaming 时能回传文本签名；非流式或 OpenAI 来源可能不保存文本签名。该结论取决于客户端版本，本项目没有逐项实测。网关只转发客户端实际提供的签名。

## 浏览器链路

浏览器侧的内容哈希、BotGuard 快照、SAPISIDHASH 签名与账号槽位处理见 [浏览器网关](browser.md#每次请求)。这些鉴权数据只保存在内存中，不会写入日志，也不会作为 `thoughtSignature` 返回。

## 验证边界

离线协议、HTTP 和浏览器脚本测试验证的是报文构造与回传。参考项目记录的真实模型实验不是本项目的实测结果；消息边界保真不保证模型能区分相邻消息，签名是否被用于推理也无法由这些测试证明。
