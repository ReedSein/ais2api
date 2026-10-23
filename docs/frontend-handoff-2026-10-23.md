# 前端修复交接：配置密钥的生效状态

日期：2026-10-23。发现基线：`dc21e6e683a8988299c16660a1f4a8a9ed7e006b`。

交给维护 WebUI 的 Harness。本次后端修复不修改 `webui/`；下面的问题仍需前端处理。此文档可以独立阅读和执行。

## 问题与影响

**[P2] 配置密钥被保存但尚未生效时，密钥页仍提示撤销/轮换成功。**

用户先保存端口等需要重启的配置，再清除或轮换配置密钥。后端把新的密钥值合并进待重启配置并写入磁盘，当前进程仍使用旧密钥。后端明确返回 `applied:false,restart_required:true`，前端却忽略响应：

- 清除提示“配置密钥已清除”，确认弹窗还承诺“客户端会立即失去访问权限”；实际旧密钥继续有效。
- 轮换提示“配置密钥已更新”，同时复制新密钥；实际新密钥在重启前会得到 401。

这是生效状态与文案的问题。数据库托管密钥的创建、改名、撤销使用另一套接口，不受这个待重启行为影响。

## 相关位置

| 文件 | 位置/用途 |
|---|---|
| `webui/src/views/KeysView.vue` | `submitConfigKey()` 约 109 行，`clearConfigKey()` 约 117 行 |
| 同上 | `guarded()` 约 45 行：回调无异常就无条件发出成功通知 |
| `webui/src/api/types.ts` | 已存在 `ConfigSaved`、`ConfigView` 类型，可直接复用 |
| `webui/src/views/ConfigView.vue` | 已有保存/待重启状态呈现，可参考其文案和交互 |
| `src/server.cpp` | `Service::control()` 的 `config.patch` 分支约 202 行 |
| 同上 | 管理路由 `/keys/config` 约 1010 行，实际转为 `config.patch` |

行号基于发现时版本；请以函数和路由名定位。

## 后端合同

配置密钥接口：

```http
PUT /api/admin/v1/keys/config
Authorization: Bearer <管理密钥>
Content-Type: application/json

{"api_key":"synthetic-new-key"}
```

```http
DELETE /api/admin/v1/keys/config
Authorization: Bearer <管理密钥>
```

两者成功时都返回 HTTP 200 和现有 `ConfigSaved`：

```ts
interface ConfigSaved {
  saved: boolean
  applied: boolean
  restart_required: boolean
}
```

| 响应 | 含义 |
|---|---|
| `saved:true, applied:true, restart_required:false` | 已保存，当前服务已使用新值 |
| `saved:true, applied:false, restart_required:true` | 已保存，当前服务仍使用旧值，需重启 |

HTTP 200 不能单独作为“当前进程已撤销/轮换”的依据。

`GET /api/admin/v1/config` 的 `config` 是当前生效配置，`saved_config` 在有待重启更改时存在，`restart_required` 指示有待生效修改。密钥值始终脱敏。`GET /api/admin/v1/keys` 的 `config_key_configured` 也反映当前生效状态；待重启的清除操作后仍显示已配置，是符合后端合同的。

## 最小复现

使用隔离的合成配置，设 `api_key=synthetic-old-key`、`admin_key=synthetic-admin-key`；不需要账号或 Google 请求。

1. 启动服务，打开配置页，把端口改为另一个可用端口并保存，暂不重启。
2. 进入 API 密钥页，清除配置密钥。
3. 观察通知与确认文案；检查该 DELETE 的响应：

   ```json
   {"saved":true,"applied":false,"restart_required":true}
   ```

4. 向原端口发送 `POST /v1/chat/completions`，请求体 `{}`：旧生成密钥得到 `400 invalid_argument`，说明通过了鉴权；随意错误密钥得到 `401 authentication`。这个请求不会进入真实生成。
5. 在新的隔离实例重复步骤 1，将步骤 2 改为轮换密钥；旧密钥仍通过鉴权，新密钥返回 401。

该行为已经通过真实 loopback HTTP 复现。可选本机证据脚本在 `build/review-management-probe/http_probe.py`；`build/` 是忽略目录，交接时不要求接收方有此脚本。

## 修复要求

1. 两个配置密钥接口都使用 `ConfigSaved` 读取结果，根据 `applied`、`restart_required` 展示状态。不要继续把它们放进无条件提示“已更新/已清除”的 `guarded()` 路径。
2. 即时生效时可以提示“配置密钥已更新/已清除”；待重启时提示“已保存，重启后生效”，并明确旧密钥在重启前仍有效。
3. 轮换后可以继续提供复制新密钥的操作，但待重启时必须说明该值尚未生效，避免用户立即替换客户端配置后误判服务故障。
4. 清除确认框去掉无条件的“立即失去访问权限”承诺。可采用条件描述，最终以服务返回的状态为准。
5. 操作后刷新当前密钥列表；若展示待保存值，和当前生效状态分开展示。页面刷新/重新进入后也能从配置接口恢复待重启提示，无需在浏览器持久化密钥明文。
6. 失败响应保持现有错误处理；不要在请求失败时展示已保存或已生效。也不要为了消除提示而自动重启服务、丢弃已保存的端口设置或改写后端生效策略。

## 验收条件

| 场景 | 预期 |
|---|---|
| 无待重启配置，轮换配置密钥 | 明确提示已生效；旧值 401、新值通过鉴权 |
| 无待重启配置，清空配置密钥 | 明确提示已生效；旧值 401 |
| 已保存端口变更，轮换配置密钥 | 显示已保存且需重启；说明旧值仍有效、新值尚未生效 |
| 已保存端口变更，清空配置密钥 | 显示已保存且需重启；不能宣称访问已立即撤销 |
| 待重启后刷新页面 | 仍能看出当前生效配置与待重启状态 |
| 用户完成重启 | 当前状态刷新；待重启提示消失，密钥行为符合保存值 |
| 托管密钥操作 | 原有即时生效行为与提示正常 |
| API 返回 4xx/5xx | 显示错误，不显示成功 |

至少覆盖两种 `ConfigSaved` 响应的前端交互测试，并执行 `npm run typecheck` 与 `npm run build`。无需使用真实 Cookie 或真实生成来验收这个问题。
