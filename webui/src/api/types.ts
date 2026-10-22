// Shapes returned by the ais2api management API (schemas/management.openapi.json).
// Most responses allow additional properties, so every field is optional unless
// the backend guarantees it.

export interface ApiErrorBody {
  error?: { code?: number | string; message?: string; status?: string; reason?: string; type?: string }
}

export interface Status {
  running?: boolean
  version?: string
  logging_healthy?: boolean
  pending_logs?: number
  uptime_seconds?: number
  active?: boolean
  active_generation_account_id?: string | null
  queued?: number
  connections?: number
  ready?: boolean
  verification?: string
}

export interface Cooldown {
  model: string
  until: number
  count: number
}

export interface Account {
  id: string
  name?: string
  email?: string
  auth_user?: string
  disabled?: boolean
  expired?: boolean
  failures?: Cooldown[]
}

export interface Rotation {
  preferred_account_id?: string | null
  next_account_id?: string | null
  active_generation_account_id?: string | null
  accounts?: Account[]
}

export interface Operation<R = Record<string, unknown>> {
  operation_id: string
  status: 'queued' | 'done' | 'failed'
  result?: R
  error?: { status?: number; code?: string; message?: string }
}

export interface ProbeResult {
  account_id: string
  status?: string
  email?: string
  ready?: boolean
  login?: boolean
  identityAmbiguous?: boolean
  regionBlocked?: boolean
  error?: { status?: number; code?: string; message?: string }
}

export interface Model {
  name?: string
  displayName?: string
  description?: string
  source?: string
  inputTokenLimit?: number
  outputTokenLimit?: number
  supportedGenerationMethods?: string[]
  [key: string]: unknown
}

export interface StatRecord {
  account: string
  model: string
  success: number
  failure: number
  usage?: Record<string, unknown>
}

export interface Key {
  id: string
  name?: string
  created?: number
  source?: 'config' | 'managed' | string
}

export interface KeyList {
  source?: string
  configured?: boolean
  config_key_configured?: boolean
  keys: Key[]
}

export interface KeyCreated {
  id: string
  name: string
  secret: string
  created: number
}

export interface Limits {
  request_bytes?: number
  object_bytes?: number
  relay_bytes?: number
  queue_size?: number
  queue_seconds?: number
  generation_seconds?: number
  stream_idle_seconds?: number
  max_attempts?: number
}

export interface Config {
  version?: number
  bind?: string
  port?: number
  api_key?: string
  cookie_import_key?: string
  cookie_import_max_accounts?: number
  cookie_import_timeout_seconds?: number
  data_dir?: string
  browser_executable?: string
  proxy?: string
  headless?: boolean
  ignore_unsupported_parameters?: boolean
  model_aliases?: Record<string, string>
  configured_models?: Array<string | Record<string, unknown>>
  model_defaults?: Record<string, unknown>
  limits?: Limits
  admin_key?: string
  admin_origins?: string[]
}

export interface ConfigView {
  config: Config
  saved_config?: Config
  config_file: string
  restart_required: boolean
}

export interface ConfigSaved {
  saved: boolean
  applied: boolean
  restart_required: boolean
}

export interface LogSource {
  id: string
  bytes: number
}

export interface JournalEntry {
  cursor: string
  record: Record<string, unknown> & { timestamp?: number; event?: string; id?: string }
}

export interface LogPage {
  entries: JournalEntry[]
  next_cursor: string
  has_more: boolean
}

export interface RawLogPage {
  source: string
  cursor?: string
  next_cursor: string
  has_more: boolean
  encoding: 'base64'
  data: string
}

export interface Capabilities {
  api_version?: string
  [key: string]: unknown
}
