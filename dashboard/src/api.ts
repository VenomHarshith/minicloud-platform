import type { CreateService, Snapshot } from './types'

const API_ROOT = '/api/v1'

export class ApiError extends Error {
  constructor(
    message: string,
    readonly status: number,
  ) {
    super(message)
  }
}

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const response = await fetch(`${API_ROOT}${path}`, {
    ...init,
    headers: {
      Accept: 'application/json',
      ...(init?.body ? { 'Content-Type': 'application/json' } : {}),
      ...init?.headers,
    },
  })
  if (!response.ok) {
    let message = `${response.status} ${response.statusText}`
    try {
      const body = (await response.json()) as { error?: string }
      if (body.error) message = body.error
    } catch {
      // Keep the HTTP status when an intermediary returned a non-JSON body.
    }
    throw new ApiError(message, response.status)
  }
  if (response.status === 204) return undefined as T
  return (await response.json()) as T
}

export const api = {
  snapshot: () => request<Snapshot>('/snapshot'),
  createService: (input: CreateService) =>
    request('/services', { method: 'POST', body: JSON.stringify(input) }),
  scaleService: (name: string, replicas: number) =>
    request(`/services/${encodeURIComponent(name)}/scale`, {
      method: 'POST',
      body: JSON.stringify({ replicas }),
    }),
  restartService: (name: string) =>
    request(`/services/${encodeURIComponent(name)}/restart`, { method: 'POST' }),
  deleteService: (name: string) =>
    request(`/services/${encodeURIComponent(name)}`, { method: 'DELETE' }),
  logs: (allocationId: string, tail = 200) =>
    request<{ lines: string[] }>(
      `/allocations/${encodeURIComponent(allocationId)}/logs?tail=${tail}`,
    ),
}
