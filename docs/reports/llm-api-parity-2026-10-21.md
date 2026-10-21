# LLM API 对照与真实请求复核

日期：2026-10-21。结论：**主要路由相同，但功能与客户端合同尚不完全一致。更新 Cookie 后，Flash-Lite 多轮、system、JSON schema、图片输入、Gemini 原生 SSE 音频及 stop，以及 Gemma OpenAI SSE 真实通过；函数调用返回 403，尚未通过完整能力验收。** 本次核对结论为不完全等价、部分核心链路可用，不是全部功能验收完成。

## 比较基准与证据等级

原项目固定到公开仓库提交 [`3e2eff888b21cc403168ceaaa1379a527d8b1a1c`](https://github.com/ReedSein/aistudio-api-termux/tree/3e2eff888b21cc403168ceaaa1379a527d8b1a1c)。本地较旧的参考目录不用于本报告。对照读取源码并离线运行其真实请求转换器，没有复制原实现或测试，也没有启动原项目重复消耗生成额度。

本项目为工作区开发版本；实际 Windows 安装程序 SHA-256 为 `d8afa160925bedb975d93e8b29521d4544856020b13678c80aae6a2118b28af3`。已核对安装目录及 Browser-Local ZIP 内的 `browser/bridge.js` 与修复源码一致。工作区尚有未提交改动，不能将结果绑定到旧 Git 提交或视作正式发行验收。

下表“代码支持”只说明有实现和离线验证路径；“真实通过”仅用于已有明确结果的请求。编码某字段不代表当前上游模型接受它。

## 路由、鉴权与模型发现

| 项目 | 对照结果 | 证据与验证范围 |
|---|---|---|
| OpenAI Chat Completions 普通/SSE | 两边都有 `/v1/chat/completions` | 原版 `api/routes_openai.py`；本项目 `src/server.cpp`；基础 Flash-Lite 真实通过见下文 |
| OpenAI 模型列表/详情 | 两边都有 `/v1/models`、`/v1/models/{id}` | 原版同文件；本项目先前真实返回 41 个发现模型，配置模型为 0 |
| Gemini 普通/SSE | 两边都有 `:generateContent`、`:streamGenerateContent` | 原版 `api/routes_gemini.py`；本项目普通文本此前通过，本轮原生 SSE 音频及 stop 通过 |
| Gemini 模型列表/详情 | 两边都有 `/v1beta/models`、`/v1beta/models/{id}` | 原版 `api/routes_models.py`；本项目先前真实通过 |
| Bearer、x-goog-api-key、x-api-key、查询 key | 两边有入口，细节不同 | 本项目 Bearer 前缀大小写及空白处理更严格；原版会规范化。原版无配置密钥时允许匿名；本项目始终拒绝无有效密钥请求 |
| 模型来源与回退 | 不同 | 原版有固定模型回退；本项目只标记 live/cache/configured，不把固定列表当验证结果 |
| Responses、旧 completions、embedding、Files、独立图片/音频、批量/实时 | 两边均无对应公开路由 | 原版模型元数据提到 countTokens/createCachedContent，实际无对应路由；不能算已实现能力 |

公开路由证据：[原版 OpenAI 路由](https://github.com/ReedSein/aistudio-api-termux/blob/3e2eff888b21cc403168ceaaa1379a527d8b1a1c/src/aistudio_api/api/routes_openai.py)、[Gemini 路由](https://github.com/ReedSein/aistudio-api-termux/blob/3e2eff888b21cc403168ceaaa1379a527d8b1a1c/src/aistudio_api/api/routes_gemini.py)、[鉴权](https://github.com/ReedSein/aistudio-api-termux/blob/3e2eff888b21cc403168ceaaa1379a527d8b1a1c/src/aistudio_api/api/dependencies.py)。

## 请求与能力差异

| 能力 | 原版当前行为 | 本项目当前行为与影响 |
|---|---|---|
| 文本、多轮、system/developer | 支持；仅开头指令提升至 systemInstruction，后置指令保留位置转换为 user | 基础文本真实通过；所有 system/developer 提升合并，后置指令语义不同 |
| 消息 name/refusal | 转为文本进入历史 | 接收部分字段但未转换内容，不能声称等价 |
| 采样、token 上限、stop | 转为 Gemini；数值字符串可转换，不合法值常忽略并记录诊断 | 有转换；严格要求数值类型，部分范围校验与原版不同；两个 token 上限同时出现会报错，原版优先 max_completion_tokens |
| n 与未知参数 | n 不为 1 时降到 1；很多不支持参数诊断后忽略 | n 不为 1 和未知字段直接报错。原版能接受的客户端请求可能在此返回 400 |
| JSON/object/schema | 支持 Gemini schema 子集；strict=true 接受但明确不保证 OpenAI strict 语义 | 支持子集；strict=true 直接拒绝；不支持的 schema 关键字报错，原版部分关键字忽略 |
| 原生配置扩展 | 接受根节点 generationConfig/generation_config、safetySettings/safety_settings | 统一放入 `ais2api.gemini`；与标准字段冲突时报错，不能直接照搬原版请求 |
| thinkingLevel/reasoning_effort | 支持，且 reasoning/thinking 别名及 xhigh/max 可降为 HIGH | 支持 thinkingLevel 及部分 reasoning_effort；上述别名和 xhigh/max 不同或拒绝 |
| thinkingBudget/includeThoughts | 没有已核实 wire 映射；接收不等于生效 | 明确拒绝；不能把两边之一描述为已实现预算控制 |
| 函数声明与 AUTO/NONE | 支持 | 代码支持；本轮 AUTO 真实请求返回 403，未通过；工具结果回传未执行 |
| required/指定函数/ANY | 接受并转换或筛选；这不能证明模型实际被强制执行 | 明确拒绝，属于兼容缺口 |
| legacy functions/function_call、function 角色 | 支持旧接口及对象/字符串 arguments | 不支持旧接口；arguments 要求 JSON 字符串 |
| 工具历史批次 | 检查未回答调用，要求完整结果，连续结果合并一轮 | 验证规则较弱；连续结果分轮，批量工具回传语义存在差异 |
| 标准 tool_call_id 签名恢复 | ID 自身编码上游 ID/签名，可无状态回传 | 按 API Key 身份查询 SQLite 注册表，默认 4,096 项/24 小时；换实例、换密钥、淘汰/过期后行为不同，也不解码原版 ID |
| OpenAI 思考/图片/签名扩展 | reasoning_content、images、reasoning_images、thought_signature 等 | 使用 `ais2api.gemini_content/gemini_delta`；原版私有字段不能直接回传。原版自身也不支持所有私有输出完整回放 |
| Gemini 原生内容块 | text、inlineData、thought/signature、函数、代码/结果 | 有对应编解码和逐块签名测试；音频/SSE 真实通过，函数、代码及逐块签名真实往返仍未验证 |
| OpenAI 图片/音频输入 | 图片、wav/mp3；额外支持 input_text/input_image、inline file/PDF 等别名 | 仅 text、对象型 image_url、input_audio；缺 OpenAI inline file/PDF 等入口，Gemini inlineData 是另一入口 |
| 内联媒体限制 | 严格 base64/文件头；单件 20 MiB、总 32 MiB、32 件 | 图片及短音频真实通过；主要限制编码后请求总量，未实现相同的解码、类型与附件数量合同 |
| 远程图片 | 公网 HTTP(S)，固定经验证 DNS 地址，不跟随重定向，不继承代理/Cookie | HTTPS 443，可使用配置代理，最多四次逐跳重验的重定向；网络合同不同 |
| 内置搜索/代码执行/Maps/URL context | Gemini 路径有编码；OpenAI 标准 tools 仅函数 | 通过 Gemini 或 ais2api 扩展有编码；模型权限和实际运行未验证，本轮不触发这些工具 |
| 生图/图片输出 | 通过生成接口与模型权限实现 | 有请求/响应编码，OpenAI 输出在原生扩展；本轮按廉价模型限制不测生图 |
| grounding/citation/logprobs 明细 | 并非全部保留；OpenAI logprobs 为 null | 也不能声称完整支持；请求参数可编码不代表有响应明细 |
| fileData、缓存 CRUD、视频生成 | 非此次等价范围；无完整公开实现 | 不支持，不计为已完成能力 |

主要源码证据：[原版请求/历史转换](https://github.com/ReedSein/aistudio-api-termux/blob/3e2eff888b21cc403168ceaaa1379a527d8b1a1c/src/aistudio_api/application/openai_request.py)、[参数转换](https://github.com/ReedSein/aistudio-api-termux/blob/3e2eff888b21cc403168ceaaa1379a527d8b1a1c/src/aistudio_api/application/openai_parameters.py)、[媒体](https://github.com/ReedSein/aistudio-api-termux/blob/3e2eff888b21cc403168ceaaa1379a527d8b1a1c/src/aistudio_api/application/openai_media.py)、[工具 ID](https://github.com/ReedSein/aistudio-api-termux/blob/3e2eff888b21cc403168ceaaa1379a527d8b1a1c/src/aistudio_api/application/openai_tools.py)。本项目对应 `src/protocol.cpp`、`src/server.cpp`、`src/store.cpp`；公开扩展见 [协议合同](../protocol.md)。

## 不联网的同输入差分

使用独立编写的 31 个合成请求，分别调用原版真实 `OpenAIChatRequest/to_gemini_request` 与 C++ `normalize_openai`。原版源码提交及源目录未修改状态在执行前校验；Python 探针阻止 DNS、socket 连接与子进程，使用显式无效代理避免导入时的本地代理自动探测。数据、诊断等导入副作用限制在工作区 build 内。未实例化原版生成客户端；C++ 探针不创建数据库或浏览器。

| 分类 | 样例数 |
|---|---:|
| 两边接受且规范化结果相同 | 5 |
| 两边接受但结果不同 | 6 |
| 原版接受、本项目拒绝 | 15 |
| 本项目接受、原版拒绝 | 4 |
| 两边拒绝 | 1 |

比较只消除 null 字段及空 generationConfig 的表示差异；6 个一致结果包含一项共同拒绝。25 个差异包含 AUTO 工具配置的空名单表示差异等不一定影响执行的项，**不是 25 个已证实缺陷，也不能计算整体兼容率**。后置 system、name 文本注入、批量工具结果分轮、未知字段/数值字符串、strict/legacy/required、无效媒体和重复工具结果等实质差异均由动态输出确认。

完整合成输入、两边实际输出及差异路径见 [差分 JSON](llm-api-differential-2026-10-21.json)。其中没有真实消息、凭据、签名或账号信息。该对照在 API 规范化层结束，不证明 wire 编码、网络传输或模型执行一致。

复核入口：

```text
cmake -S . -B build/windows -DAIS2API_BUILD_PROTOCOL_PROBE=ON
cmake --build build/windows --target ais2api_protocol_probe
python tests/llm_api_differential.py --cpp-probe build/windows/ais2api_protocol_probe.exe --upstream-python .tools/llm-audit-env/Scripts/python.exe --upstream .analysis/upstream-llm-20261021 --report build/llm-api-differential-2026-10-21.json
```

探针默认不构建，不进入安装包；Python 原版依赖只位于隔离开发环境，不增加发布后端依赖。此次环境为 Python 3.13、pydantic 2.12.5、curl_cffi 0.16.3、FastAPI 0.156.4、httpx 0.28.1。其他平台可以替换构建目录和 Python 路径运行。

## 响应、流式与错误

两边都输出标准 Chat Completions envelope、工具调用、usage 和 finish reason；正常 OpenAI SSE 有结束事件，include_usage 时有 choices 为空的 usage 块。原生 Gemini SSE 不使用 `[DONE]`。中途失败不应以另一次生成续接成同一回复。本项目的慢消费者、取消、缓冲和错误回归使用合成上游，不能当作真实模型测试。

仍有需要修正或补测的差异：

- 原版只输出一个候选；本项目不能请求 n>1，但可保留上游实际返回的多个候选。
- 原版检查有效结束原因及完成状态，未知结束原因报错。本项目 OpenAI 将其他原因归为 stop/tool_calls；仅验证 JSON 框架闭合不足以证明模型完整结束，存在误报成功风险。
- 原版将流式工具调用延迟到生成完成后再携带最终签名发出；本项目按事件输出完整 arguments，事件顺序与签名到达时序需要真实覆盖。
- 原版 Gemini 当前也逐块保存顺序、思考、签名和代码结构，不能沿用旧源码版本“会丢失签名”的结论。本项目在缓存 token 等 usage 字段上有不同保留范围。

证据：[原版响应](https://github.com/ReedSein/aistudio-api-termux/blob/3e2eff888b21cc403168ceaaa1379a527d8b1a1c/src/aistudio_api/application/openai_response.py)、[完成状态校验](https://github.com/ReedSein/aistudio-api-termux/blob/3e2eff888b21cc403168ceaaa1379a527d8b1a1c/src/aistudio_api/application/generation_service.py)、[Gemini 内容块](https://github.com/ReedSein/aistudio-api-termux/blob/3e2eff888b21cc403168ceaaa1379a527d8b1a1c/src/aistudio_api/api/responses.py)。

## 真实请求与会话更新

此前同日完成的修复及真实生成记录见 [XHR 初始化修复](generation-bootstrap-2026-10-21.md)：Windows `gemini-3.1-flash-lite` 的 OpenAI 普通生成、OpenAI SSE、Gemini 普通生成和 WSL 调 Windows 的多轮请求均 HTTP 200，正确得到 OK/NEXT。它们证明基础链路曾可用，**不能替代当前会话或高级能力验收**。

本轮新增 `tests/live_llm_audit.py`，限定最多五个生成 HTTP 请求，只选择真实发现的 Flash-Lite/Gemma，检查有效配置中的模型别名，服务临时限制每请求一次尝试。计划合并覆盖：system/历史/JSON schema/图片、Gemma SSE、函数调用及标准客户端回传、Gemini SSE/短音频/stop。未使用较高成本模型，也未触发生图、搜索或代码执行。

最初驱动在生成前的 `/v1/models` 预检失败。前两次未记录细分类别，补充脱敏诊断后再次得到 HTTP 401、`session_expired`；**这些预检共发送零个生成请求**。独立新 BrowserContext 注入当时已存 Cookie 后，六次间隔采样均处于 Google 登录页，没有触发 Run。该阶段记录保留，不能反过来解释后续更新后的会话状态。

该阶段驱动最后一次自动恢复报告失败，随后独立 CLI 确认服务已停止，符合当时测试前状态。异常保留在 [初次预检脱敏报告](llm-api-live-audit-2026-10-21.json)，没有改写成自动恢复成功。用户随后推送时原版脚本提示结果不明确；检查发现服务已停止，没有新导入记录。服务启动后用户重新推送并确认导入成功。

更新后真实结果如下。初批四个请求有完整的四条 attempts=1 日志；补充一次 Gemma 也只有一条 attempts=1 日志。总计 **5 个生成 HTTP 请求、5 次实际尝试**，全部使用 `gemini-3.1-flash-lite` 或 `gemma-4-26b-a4b-it`。没有发送工具结果回传请求，也没有向原项目重复发送生成。列表、账号身份探测与失败的启动/预检不计作生成；浏览器占位初始化请求由本地拦截，不触发上游生成。

| 实际生成 | 结果与可证明范围 |
|---|---|
| Flash-Lite OpenAI 普通：system + 多轮 + JSON schema + 红色图片 | HTTP 200，系统码、历史标记及 red 均正确，JSON 字段集合、usage、finish=stop 通过；约 9.42 秒 |
| Gemma OpenAI SSE，max_tokens=64 | 传输、稳定 ID/created、usage、finish、DONE 校验完成，但精确正文失败。统计显示 61 个输出 token 全为思考，不能当作可见正文成功 |
| Flash-Lite AUTO 函数调用 | HTTP 403、permission_denied，未通过；没有伪造调用或继续提交工具结果 |
| Flash-Lite Gemini 原生 SSE：短合成音频 + stop | HTTP 200，正确判断无语音的音调；输出在指定 stop 前终止，无后缀；3 个 JSON 事件、usage/finish/稳定 ID 通过，原生无 DONE；约 10.33 秒 |
| Gemma OpenAI SSE，补充 max_tokens=512 | HTTP 200，正文精确为 OK；7 个 JSON 事件、usage、finish=stop、DONE、稳定 ID/created 通过；6 输入 + 72 输出 = 78 token，其中 71 为思考；约 11.52 秒 |

Gemma 首次问题与思考消耗输出预算相符；提高上限后同模型正确返回正文，不支持把首次断言失败归为传输错误。512 是请求上限，实际输出仅 72 token。不能从单个采样证明 temperature/top_p 的统计分布，也不能从简单红色图片/短音调证明所有媒体格式与长输入均可用。

初批证据见 [更新 Cookie 后的四请求报告](llm-api-live-updated-cookie-2026-10-21.json)，补充见 [Gemma 第五请求报告](llm-api-gemma-supplement-2026-10-21.json)。补充前曾遇到 Windows 控制管道排空期间的回执错误及模型列表预检 HTTP 错误，这些尝试均未发送生成；探针不能把观察失败当作服务已停止。驱动增加了有界停止确认，最终补充请求及恢复成功。其间两个账号仅身份探测均 ready，无模型生成。

工具 403 的原因尚未证实：不能据此断定账号没有工具权限，也不能断定原版同一请求一定通过。静态检查确认基础函数声明的 wire 形状相同；另有两项值得后续隔离验证的差异：原版在工具请求设置 slot 13 的 location/timezone 伴随字段，而本项目只继承模板值；原版逐请求刷新 SAPISIDHASH/账号头，本项目复用捕获头。这些只是候选原因，尚无真实对照证明。原版自身也有针对偶发 403 刷新认证的恢复逻辑。

相关原版证据：[wire 编码](https://github.com/ReedSein/aistudio-api-termux/blob/3e2eff888b21cc403168ceaaa1379a527d8b1a1c/src/aistudio_api/infrastructure/gateway/wire_codec.py)、[请求认证头](https://github.com/ReedSein/aistudio-api-termux/blob/3e2eff888b21cc403168ceaaa1379a527d8b1a1c/src/aistudio_api/infrastructure/gateway/transport.py)、[账号恢复](https://github.com/ReedSein/aistudio-api-termux/blob/3e2eff888b21cc403168ceaaa1379a527d8b1a1c/src/aistudio_api/application/account_orchestrator.py)。

本次没有对未确认的 403 反复试错，没有放宽身份检查或伪造快照、签名、生成内容。完整工具闭环、内置工具、生图、逐块思考签名真实往返、长输入与长期运行仍未验证。

结束前通过本地控制通道确认：服务 running=true、active=false、queued=0，有效 max_attempts 恢复为原配置的 3；测试期间使用的是 1。没有修改配置文件或保留测试覆盖参数。

## 本机复核与优先事项

- Windows 正常权限：105 个 C++ 测试、1,599 条断言全部通过；Node 桥接 24 项通过。本轮受限环境中的本地 TLS 失败未作为产品失败，正常环境复核通过；受限测试进程已终止。
- 真测驱动的七项离线保护测试通过：模型家族限制、昂贵别名拒绝、双协议共享请求上限、有效环境配置覆盖、停止回复丢失及管道排空时的状态确认。没有在这些离线测试中访问模型服务。
- Browser-Local ZIP：767 个文件，逐文件浏览器清单与归档 SHA-256 检查通过；包内桥接脚本与源码相同。该包仍是开发整合包。
- Debian GCC、Clang ASan/UBSan 的既有 105 用例/1,591 断言记录保留；本轮没有重新运行。没有新增真实 Debian/Termux、两小时稳定性或同负载性能对照数据。

优先级：隔离定位函数请求 403，完成工具调用/回传闭环；修正未知结束状态的成功判定、工具批次/签名时序及媒体校验；随后根据客户端需求决定增加原版扩展兼容层、legacy 函数参数及参数降级策略。required/strict 等没有可靠保证的能力应继续明确标示限制，不能为表面兼容宣称已生效。以上是本次核对发现的后续工作，不是已完成的修复。
