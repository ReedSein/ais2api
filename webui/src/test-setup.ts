import { Storage } from 'happy-dom'

// Node.js 25+ defines its own localStorage and sessionStorage globals, and
// Vitest does not replace globals that already exist, so happy-dom's storage
// never reaches the tests (localStorage is undefined without
// --localstorage-file). Give every test file fresh browser storage.
for (const name of ['localStorage', 'sessionStorage'] as const) {
  Object.defineProperty(globalThis, name, { value: new Storage(), configurable: true, writable: true })
}
