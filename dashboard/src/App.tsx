import { FormEvent, useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { api } from './api'
import type { Allocation, CreateService, Service, Snapshot } from './types'

const emptyDeploy: CreateService = {
  name: '',
  image: '',
  replicas: 1,
  cpuMillis: 250,
  memoryMb: 256,
  containerPort: 8080,
  healthPath: '/health',
}

function relativeTime(value: string) {
  const seconds = Math.max(0, Math.round((Date.now() - new Date(value).getTime()) / 1000))
  if (seconds < 60) return `${seconds}s ago`
  if (seconds < 3600) return `${Math.floor(seconds / 60)}m ago`
  return `${Math.floor(seconds / 3600)}h ago`
}

function PercentBar({ used, total, label }: { used: number; total: number; label: string }) {
  const percent = total > 0 ? Math.min(100, Math.round((used / total) * 100)) : 0
  return (
    <div className="resource" aria-label={`${label}: ${percent}% allocated`}>
      <div className="resource-row"><span>{label}</span><strong>{percent}%</strong></div>
      <div className="track"><span style={{ width: `${percent}%` }} /></div>
      <small>{used.toLocaleString()} / {total.toLocaleString()}</small>
    </div>
  )
}

function StateBadge({ state }: { state: string }) {
  const tone = ['running', 'ready', 'healthy'].includes(state) ? 'good' :
    ['pending', 'starting', 'unknown'].includes(state) ? 'warn' : 'bad'
  return <span className={`badge ${tone}`}><i />{state.replace('_', ' ')}</span>
}

export default function App() {
  const [snapshot, setSnapshot] = useState<Snapshot | null>(null)
  const [error, setError] = useState('')
  const [busy, setBusy] = useState(false)
  const [deployOpen, setDeployOpen] = useState(false)
  const [deploy, setDeploy] = useState<CreateService>(emptyDeploy)
  const [selected, setSelected] = useState<Allocation | null>(null)
  const [logs, setLogs] = useState<string[]>([])
  const hasSnapshot = useRef(false)

  const refresh = useCallback(async (quiet = false) => {
    try {
      const value = await api.snapshot()
      hasSnapshot.current = true
      setSnapshot(value)
      setError('')
    } catch (caught) {
      if (!quiet || !hasSnapshot.current) {
        setError(caught instanceof Error ? caught.message : 'Request failed')
      }
    }
  }, [])

  useEffect(() => {
    void refresh()
    const timer = window.setInterval(() => void refresh(true), 3000)
    return () => window.clearInterval(timer)
  }, [refresh])

  const openLogs = async (allocation: Allocation) => {
    setSelected(allocation)
    setLogs(['Loading container logs…'])
    try {
      const result = await api.logs(allocation.id)
      setLogs(result.lines.length ? result.lines : ['No log lines have been produced.'])
    } catch (caught) {
      setLogs([caught instanceof Error ? caught.message : 'Unable to load logs'])
    }
  }

  const act = async (action: () => Promise<unknown>) => {
    setBusy(true)
    try {
      await action()
      await refresh()
    } catch (caught) {
      setError(caught instanceof Error ? caught.message : 'Operation failed')
    } finally {
      setBusy(false)
    }
  }

  const submitDeploy = (event: FormEvent) => {
    event.preventDefault()
    void act(async () => {
      await api.createService(deploy)
      setDeploy(emptyDeploy)
      setDeployOpen(false)
    })
  }

  const health = snapshot?.overview
  const readiness = health && health.desiredReplicas > 0
    ? Math.round((health.readyReplicas / health.desiredReplicas) * 100)
    : 100
  const grouped = useMemo(() => {
    const byService = new Map<string, Allocation[]>()
    snapshot?.allocations.forEach((allocation) => {
      byService.set(allocation.serviceName, [...(byService.get(allocation.serviceName) ?? []), allocation])
    })
    return byService
  }, [snapshot])

  return (
    <div className="shell">
      <aside>
        <div className="brand"><div className="mark">MC</div><div><strong>MiniCloud</strong><small>container platform</small></div></div>
        <nav aria-label="Primary navigation">
          <a className="active" href="#overview">Overview</a>
          <a href="#services">Services</a>
          <a href="#nodes">Worker nodes</a>
          <a href="#activity">Activity</a>
          <a href="http://127.0.0.1:9090" target="_blank" rel="noreferrer">Prometheus ↗</a>
        </nav>
        <div className="runtime-status"><span className={health?.healthy ? 'pulse' : 'pulse offline'} /> <div><strong>{health?.healthy ? 'Control plane healthy' : 'Connecting…'}</strong><small>auto-refresh · 3 seconds</small></div></div>
      </aside>

      <main>
        <header>
          <div><p className="eyebrow">Operations console</p><h1>Cluster overview</h1><p>Desired state, placement, and live container health.</p></div>
          <button className="primary" onClick={() => setDeployOpen(true)}>+ Deploy service</button>
        </header>

        {error && <div className="alert" role="alert"><strong>Platform unavailable.</strong> {error}<button onClick={() => void refresh()}>Retry</button></div>}

        <section className="stats" id="overview">
          <article><span>Services</span><strong>{health?.services ?? '—'}</strong><small>{health?.desiredReplicas ?? 0} desired replicas</small></article>
          <article><span>Ready containers</span><strong>{health ? `${health.readyReplicas}/${health.desiredReplicas}` : '—'}</strong><small>{readiness}% converged</small></article>
          <article><span>Worker nodes</span><strong>{health ? `${health.readyNodes}/${health.totalNodes}` : '—'}</strong><small>{health?.pendingAllocations ?? 0} pending allocations</small></article>
          <article><span>Restarts</span><strong>{health?.restartCount ?? '—'}</strong><small>across current allocations</small></article>
        </section>

        <section className="panel" id="services">
          <div className="panel-heading"><div><p className="eyebrow">Desired state</p><h2>Services</h2></div><span>{snapshot?.services.length ?? 0} total</span></div>
          <div className="table-wrap">
            <table>
              <thead><tr><th>Service</th><th>Image</th><th>Replicas</th><th>Resources</th><th>Generation</th><th><span className="sr-only">Actions</span></th></tr></thead>
              <tbody>
                {snapshot?.services.map((service: Service) => (
                  <tr key={service.id}>
                    <td><strong>{service.name}</strong><small>:{service.containerPort}{service.healthPath}</small></td>
                    <td className="mono" title={service.image}>{service.image}</td>
                    <td><span className={service.readyReplicas === service.desiredReplicas ? 'ready-count' : 'pending-count'}>{service.readyReplicas}</span> / {service.desiredReplicas}</td>
                    <td><small>{service.cpuMillis}m CPU · {service.memoryMb} MiB</small></td>
                    <td>v{service.generation}</td>
                    <td><div className="actions"><button disabled={busy} onClick={() => void act(() => api.scaleService(service.name, service.desiredReplicas + 1))}>+1</button><button disabled={busy || service.desiredReplicas === 0} onClick={() => void act(() => api.scaleService(service.name, service.desiredReplicas - 1))}>−1</button><button disabled={busy} onClick={() => void act(() => api.restartService(service.name))}>Restart</button></div></td>
                  </tr>
                ))}
                {!snapshot?.services.length && <tr><td colSpan={6} className="empty">No services yet. Deploy the included echo example or submit your own image.</td></tr>}
              </tbody>
            </table>
          </div>
        </section>

        <div className="grid">
          <section className="panel" id="nodes">
            <div className="panel-heading"><div><p className="eyebrow">Capacity</p><h2>Worker nodes</h2></div></div>
            <div className="node-list">
              {snapshot?.nodes.map((node) => <article className="node" key={node.id}>
                <div className="node-head"><div><strong>{node.name}</strong><small>heartbeat {relativeTime(node.lastHeartbeat)}</small></div><StateBadge state={node.status} /></div>
                <PercentBar label="CPU millicores" used={node.cpuAllocatedMillis} total={node.cpuCapacityMillis} />
                <PercentBar label="Memory MiB" used={node.memoryAllocatedMb} total={node.memoryCapacityMb} />
                <div className="labels">{Object.entries(node.labels).map(([key, value]) => <span key={key}>{key}={value}</span>)}</div>
              </article>)}
              {!snapshot?.nodes.length && <p className="empty">Waiting for a worker to register through gRPC.</p>}
            </div>
          </section>

          <section className="panel" id="activity">
            <div className="panel-heading"><div><p className="eyebrow">Audit trail</p><h2>Recent activity</h2></div></div>
            <div className="events">{snapshot?.events.map((event) => <article key={event.id}><span className="event-dot" /><div><strong>{event.type}</strong><p>{event.message}</p><small>{relativeTime(event.createdAt)} · {event.aggregateId.slice(0, 12)}</small></div></article>)}
              {!snapshot?.events.length && <p className="empty">Reconciliation events will appear here.</p>}
            </div>
          </section>
        </div>

        <section className="panel">
          <div className="panel-heading"><div><p className="eyebrow">Observed state</p><h2>Allocations</h2></div></div>
          <div className="allocations">
            {snapshot?.services.flatMap((service) => (grouped.get(service.name) ?? []).map((allocation) => <article key={allocation.id}>
              <div><strong>{allocation.serviceName}-{allocation.replica}</strong><small className="mono">{allocation.containerId?.slice(0, 12) ?? 'not created'}</small></div>
              <StateBadge state={allocation.state} />
              <div><small>Node</small><span>{allocation.nodeName ?? 'unscheduled'}</span></div>
              <div><small>Endpoint</small><span className="mono">{allocation.endpoint ?? '—'}</span></div>
              <div><small>Restarts</small><span>{allocation.restartCount}</span></div>
              <button onClick={() => void openLogs(allocation)}>Logs</button>
            </article>))}
            {!snapshot?.allocations.length && <p className="empty">No allocations have been planned.</p>}
          </div>
        </section>
      </main>

      {deployOpen && <div className="modal-backdrop" onMouseDown={() => setDeployOpen(false)}><section className="modal" role="dialog" aria-modal="true" aria-labelledby="deploy-title" onMouseDown={(event) => event.stopPropagation()}>
        <div className="panel-heading"><div><p className="eyebrow">Desired state</p><h2 id="deploy-title">Deploy a container</h2></div><button className="icon" onClick={() => setDeployOpen(false)} aria-label="Close">×</button></div>
        <form onSubmit={submitDeploy}>
          <label>Service name<input required pattern="[a-z][a-z0-9-]{0,62}" value={deploy.name} onChange={(event) => setDeploy({ ...deploy, name: event.target.value })} placeholder="orders-api" /></label>
          <label>Container image<input required value={deploy.image} onChange={(event) => setDeploy({ ...deploy, image: event.target.value })} placeholder="ghcr.io/example/orders@sha256:…" /></label>
          <div className="form-grid"><label>Replicas<input type="number" min="0" max="50" value={deploy.replicas} onChange={(event) => setDeploy({ ...deploy, replicas: Number(event.target.value) })} /></label><label>Container port<input type="number" min="1" max="65535" value={deploy.containerPort} onChange={(event) => setDeploy({ ...deploy, containerPort: Number(event.target.value) })} /></label></div>
          <div className="form-grid"><label>CPU millicores<input type="number" min="10" value={deploy.cpuMillis} onChange={(event) => setDeploy({ ...deploy, cpuMillis: Number(event.target.value) })} /></label><label>Memory MiB<input type="number" min="16" value={deploy.memoryMb} onChange={(event) => setDeploy({ ...deploy, memoryMb: Number(event.target.value) })} /></label></div>
          <label>HTTP health path<input required value={deploy.healthPath} onChange={(event) => setDeploy({ ...deploy, healthPath: event.target.value })} /></label>
          <p className="notice">Use immutable image digests for reproducible deployments. MiniCloud never builds or pulls from private registries without Docker already being authenticated.</p>
          <div className="form-actions"><button type="button" onClick={() => setDeployOpen(false)}>Cancel</button><button className="primary" disabled={busy}>{busy ? 'Submitting…' : 'Deploy'}</button></div>
        </form>
      </section></div>}

      {selected && <div className="drawer-backdrop" onMouseDown={() => setSelected(null)}><aside className="drawer" onMouseDown={(event) => event.stopPropagation()}>
        <div className="panel-heading"><div><p className="eyebrow">Container output</p><h2>{selected.serviceName}-{selected.replica}</h2></div><button className="icon" onClick={() => setSelected(null)} aria-label="Close logs">×</button></div>
        <pre>{logs.join('\n')}</pre>
      </aside></div>}
    </div>
  )
}
