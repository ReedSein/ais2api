# 真 role 与客户端思考签名

两份参考文档中的功能通过现有 Gemini 与 OpenAI 入口提供，无需新增开关。每次请求由客户端提交完整历史；服务不保存聊天历史、不为手写 assistant 条目补签名，也不插入分隔消息。

## Gemini 原生入口

`/v1beta/models/MODEL:generateContent` 和 `:streamGenerateContent` 保留每条 content 的角色、消息边界与部件顺序。连续 user/user、model/model 原样编码。systemInstruction 位于独立槽位；未声明 tools 时不会自动启用搜索；模型 id 以 `-search` 结尾时除外，见 [搜索模型](protocol.md#search-models)。

下一轮应把响应候选的整个 content 放回历史，而不是仅提取可见文字：

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

例子中的签名文字仅说明字段位置，不能作为有效签名发送。签名是不透明的模型输出，应跟随原部件，不修改、不移动、不伪造。编辑可见正文时可以保留其余原部件；删除消息时连同该消息的思考与签名一起删除。手写 assistant 直接使用普通 model 文本。签名跨模型复用的效果没有验证。

流式与非流式输出均保留独立签名部件，以及函数调用、代码执行、执行结果的部件顺序。仅相邻且均无签名的同类文本增量会合并。未知部件数据或损坏的代码/函数部件返回 502，避免静默丢失上下文。

## OpenAI 兼容入口

`/v1/chat/completions` 保留普通消息边界。开头连续的 system/developer 转成独立系统提示；对话开始后出现的 system/developer 在原位置转成 user 文本，不添加角色标签。连续 tool 结果合为同一条 user 消息内的多个结果部件。

assistant 可使用 `reasoning_content`、`content` 与 `thought_signature`（输入也接受 `signature`）带回思考、正文和独立文本签名。响应消息及流式 delta 都提供这些兼容字段。若需要保留交错的原生部件，请使用 Gemini 原生入口，或原样回传 `message.ais2api.gemini_content`；兼容字段与该原生内容同时提供时会检查一致性。

带签名的工具调用把原始调用 ID、签名和区分调用的 nonce 编码在 `call_gemini_...` ID 内。客户端原样保存 tool_calls[].id 和 tool_call_id 即可；服务从历史中的 ID 恢复签名，无需服务端保存签名。不要截断、重写或解读这个 ID。原调用未提供 ID 时，转回 Gemini 后仍不添加 ID。未编码的旧调用 ID 仍接受，但服务不会补回旧版本保存在数据库中的签名。

参考文档对 SillyTavern/Luker 的读码结论是：Google AI Studio 源开启 Streaming 时能回传文本签名；非流式或 OpenAI 源可能不保存文本签名。此结论取决于客户端版本，本项目没有在客户端逐项实测。网关仅转发客户端实际提供的签名。

## 浏览器链路与验证边界

浏览器就绪后，网关对最终 JSPB contents 计算 SHA-256：每个文本或内联数据贡献对应值，其余部件贡献空串，按单空格连接；展开的函数结果附件也计入。每次请求重新调用已捕获的页面快照服务，不复用 BotGuard 票据。生成和模型查询通过当前隔离页面的 CDP Cookie 缓存，以新时间戳计算 SAPISIDHASH、SAPISID1PHASH、SAPISID3PHASH，支持 HttpOnly Cookie。缓存缺失、Cookie 到期或超过 60 秒时刷新，切号或关闭时清除；无法新签名时回退该页面捕获的 Authorization，两者皆无则本地失败。填写已验证账号的 X-Goog-AuthUser，并拒绝捕获头中的账号冲突。鉴权数据仅驻留内存，不会写入日志或作为 thoughtSignature 返回。

预热继续拦截 SavePrompt/CreatePrompt，并尽力启用 Temporary chat；成功或失败后停止探针、恢复输入框。临时聊天控件和页面结构可能随上游变化。

离线协议、HTTP 和浏览器脚本测试验证报文构造与回传。参考项目记录的真实模型实验不是本项目的实测结果；消息边界保真不保证模型区分相邻消息，签名是否被用于推理也无法由这些测试证明。
