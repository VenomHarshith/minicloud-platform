export type Service = {
  id: string
  name: string
  image: string
  desiredReplicas: number
  readyReplicas: number
  cpuMillis: number
  memoryMb: number
  containerPort: number
  healthPath: string
  generation: number
  createdAt: string
}

export type Node = {
  id: string
  name: string
  status: 'ready' | 'not_ready' | 'draining' | 'unknown'
  cpuCapacityMillis: number
  cpuAllocatedMillis: number
  memoryCapacityMb: number
  memoryAllocatedMb: number
  lastHeartbeat: string
  labels: Record<string, string>
}

export type Allocation = {
  id: string
  serviceName: string
  replica: number
  nodeName: string | null
  state: string
  containerId: string | null
  restartCount: number
  endpoint: string | null
  updatedAt: string
}

export type EventRecord = {
  id: number
  type: string
  aggregateId: string
  message: string
  createdAt: string
}

export type Overview = {
  healthy: boolean
  services: number
  desiredReplicas: number
  readyReplicas: number
  readyNodes: number
  totalNodes: number
  pendingAllocations: number
  restartCount: number
  schedulerLeader: string
}

export type Snapshot = {
  overview: Overview
  services: Service[]
  nodes: Node[]
  allocations: Allocation[]
  events: EventRecord[]
}

export type CreateService = {
  name: string
  image: string
  replicas: number
  cpuMillis: number
  memoryMb: number
  containerPort: number
  healthPath: string
}
