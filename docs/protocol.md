# Protocol contract

The implementation is new C++ code. The reference project's `docs/WIRE_SPECIFICATION.md` is used only as an upstream protocol description; its implementation and test fixtures are not reused. Protocol facts are centralized in `src/protocol.cpp`.

## Request and response representation

Both API adapters normalize to a standard Gemini request object. The browser transport receives a newly encoded JSPB request based on the captured live template. Known generation fields, contents, model, signature, system instruction, tools and cached-content fields are replaced or cleared for every request. Unknown template fields are retained for upstream compatibility. Model aliases resolve before encoding.

Supported Gemini content payloads are text, inlineData, functionCall, functionResponse, executableCode and codeExecutionResult. Parts retain their individual thought flag and thoughtSignature. Request FunctionCall/Response use upstream slots 10/11; response slots 3/4 are read first, with 10/11 fallbacks. Function values use typed protobuf Struct/Value encoding, including null, nested objects, booleans, empty lists and single-item lists. fileData and the Files API are explicitly unsupported.

thinkingLevel uses the verified upstream [mode=1, null, null, level] representation. thinkingBudget and includeThoughts are explicitly rejected because the reference codec supplies no verified mapping for them.

Generation settings include sampling, stop sequences, token limits, response MIME/schema, penalties, logprob request flags, text/image modalities, thinking configuration, media resolution and image aspect/size. Requesting logprobs does not promise returned logprob detail: that upstream response field is not documented. Supplemental protocol facts from the reference typed wire definitions establish code as `[languageCode, source]` and execution results as `[outcomeCode, output]`; these enums are preserved, with compatibility for older bare-text responses. Function-response text/inlineData attachments expand into adjacent content parts. Request thought flags use slot 12, avoiding collision with request functionCall slot 10.

Schema encoding covers concrete string/number/integer/boolean/array/object types, nullable type arrays, nested properties/items, enum/required, sizes, ranges, pattern, example, oneOf/anyOf/allOf/not and propertyOrdering. Unsupported schema keywords (including `$ref`, additionalProperties, title and default) fail locally instead of being silently dropped. Unions still require a concrete type or a type inferred from their first branch. Strict OpenAI JSON-schema/function mode is rejected because the upstream cannot guarantee those semantics.

Tools include function declarations, code execution, Google search, Google Maps and URL context. `parametersJsonSchema` is accepted as an alternative to parameters, and supplying both fails. NONE removes all tools; allowedFunctionNames filters custom functions. ANY / OpenAI required / named tool choice and VALIDATED are rejected locally: the private upstream has no verified forced-call or validated-decoding field. The reference implementation only filtered tools for these modes; this implementation does not present filtering as forced execution.

## OpenAI Chat Completions

Standard user/assistant/system/developer/tool messages map to Gemini contents and system instruction. Standard text, image_url data URLs and input_audio wav/mp3 are supported. Remote image URLs must be fetched by the server's media loader before normalization; the protocol adapter never fetches URLs. Audio and image support ultimately depends on the selected model. Function tools, tool_choice, JSON response formats, token/sampling controls and reasoning_effort are translated. Only n=1 is supported. Unsupported fields fail with invalid_argument; they are not accepted as implied functionality.

Assistant tool call IDs are preserved. If the upstream omits an ID, an ID is derived from response ID, function payload and a newly allocated call ordinal. During streaming, the authenticated principal's persistent response registry assigns increasing ordinals and indices across chunks; identical idless calls in different chunks are separate invocations. Repeated explicit upstream IDs retain their existing index. The HTTP server always supplies this registry; stateless codec helpers cannot maintain call ordinals across independent stream conversions. A response may register at most 4096 distinct calls. The Store associates tool metadata and thoughtSignature with the authenticated principal, enabling subsequent standard tool messages without exposing a credential or requiring clients to modify argument JSON. A native assistant content envelope is also available for stateless signature round trips. Function argument JSON must be an object; tool-result nonobject/string content is wrapped as `{ "result": ... }`.

Standard OpenAI output contains content, tool_calls, finish_reason and token usage. Nonstandard data is confined to the `ais2api` namespace. Rich assistant outputs include their entire native Gemini content for round trips; stream deltas include the normalized native chunk. Thinking text is never mixed into visible standard content. Images and code parts remain in the native extension. Complete tool calls are emitted as complete argument strings; the transport does not fabricate partial argument fragments.

## ais2api extensions, version 1

The schema is [protocol-extensions.schema.json](../schemas/protocol-extensions.schema.json). Every extension name outside this contract is rejected.

Request capabilities absent from standard OpenAI fields are supplied at the root:

```json
{
  "ais2api": {
    "gemini": {
      "generationConfig": {"responseModalities": ["TEXT", "IMAGE"]},
      "tools": [{"googleSearch": {}}]
    }
  }
}
```

The allowed gemini keys are generationConfig, tools, toolConfig, safetySettings and systemInstruction. Fields already supplied through standard request fields cause an error, even if their values are equal. generationConfig may be merged only when individual subkeys do not overlap.

An assistant message can round-trip native content with `message.ais2api.gemini_content`:

```json
{
  "role": "assistant",
  "content": "answer",
  "ais2api": {
    "gemini_content": {"role": "model", "parts": [{"text": "answer", "thoughtSignature": "opaque-signature"}]}
  }
}
```

Standard visible text and tool_calls, when supplied beside this envelope, must agree. The envelope is authoritative for native images/thoughts/signatures. When an upstream function call has no ID, the output envelope adds the same derived `functionCall.id` used by the standard `tool_calls` entry. This is done on an extension copy; all other native fields, part order and each part's original signature remain unchanged. Returning the generated standard fields and native envelope together therefore passes the same strict ID/name/argument agreement checks. It is supported only on assistant messages. Native extension data should be retained as opaque data by clients; signatures must not be synthesized or edited.

OpenAI stream chunks carry `choices[].delta.ais2api.gemini_delta`, containing the entire normalized Gemini chunk. Function calls missing an upstream ID receive the same derived ID in this extension copy as in their standard delta; existing IDs and signatures are preserved. Metadata-only chunks without a choice carry `ais2api.gemini_delta` at the root. This is intentionally not a new root reasoning_content or custom standard field.

## Streaming and aggregation

The upstream framing is an optional `)]}'` XSSI prefix followed by an outer array of batches of response chunk arrays. The bounded parser emits each depth-three chunk and discards its bytes before consuming the next. Network split points, UTF-8 code units, JSON escapes and strings containing brackets can occur anywhere. Invalid preambles, invalid chunk JSON/UTF-8, excess nesting, oversized chunks, malformed outer separators and truncated final data fail explicitly. The parser buffers only the current chunk plus a two-byte outer framing state, not the full response. Successful `finish()` requires the complete outer arrays to close.

Aggregation is bounded by serialized response bytes. Ordinary adjacent text parts with matching thought flags may be concatenated; signed parts and other native payloads remain separate. Usage and finish metadata replace previous snapshots rather than being summed. A failing size check leaves the prior aggregate unchanged. Content gating ignores response ID, usage and finish-only metadata, allowing the orchestrator to distinguish metadata from the first visible/model-action payload.

Finish reasons map through all documented upstream codes. OpenAI maps token exhaustion to length, safety/recitation failures to content_filter and function completion to tool_calls; other completions map to stop. candidatesTokenCount includes visible completion plus thinking tokens, while thoughtsTokenCount remains separately available; OpenAI exposes reasoning_tokens and cached_tokens details. HTTP/gRPC error classification and retry decisions belong to the runtime/transport, outside this codec.

## Validation scope

The HTTP adapter follows the [official OpenAI streaming contract](https://developers.openai.com/api/reference/resources/chat/subresources/completions/streaming-events): when include_usage is requested, ordinary chunks have null standard usage and a separate final chunk has empty choices with the actual available usage before DONE. Without that option, standard usage is omitted. The native extension may still preserve upstream usage metadata. Stream IDs and created timestamps remain stable, and a midstream failure emits an error without DONE.

Catch2 protocol tests use independent synthetic samples. They cover arbitrary byte boundaries, malformed/truncated streams, bounds, typed function values, per-part signatures, schema recursion, request-template replacement, extension conflicts, multi-message tools and native OpenAI round trips. Regression cases additionally cover repeated identical idless calls across stream chunks, stable explicit-ID indices, signed and unsigned calls without signature rebinding, and generated standard/native tool messages returned together with strict conflict rejection. These are protocol/adapter tests, not proof that an authenticated live upstream model supports each capability. Live browser and platform qualification must be reported separately.
