/// <reference types="vite/client" />

interface ImportMetaEnv {
  readonly VITE_PUBLIC_DEMO?: string
}

interface ImportMeta {
  readonly env: ImportMetaEnv
}
