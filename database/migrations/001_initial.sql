BEGIN;

CREATE TABLE IF NOT EXISTS schema_migrations (
    version INTEGER PRIMARY KEY,
    description TEXT NOT NULL,
    applied_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS services (
    service_id UUID PRIMARY KEY,
    name VARCHAR(63) NOT NULL UNIQUE,
    image TEXT NOT NULL,
    desired_replicas INTEGER NOT NULL CHECK (desired_replicas BETWEEN 0 AND 50),
    cpu_millis INTEGER NOT NULL CHECK (cpu_millis BETWEEN 10 AND 128000),
    memory_mb INTEGER NOT NULL CHECK (memory_mb BETWEEN 16 AND 1048576),
    container_port INTEGER NOT NULL CHECK (container_port BETWEEN 1 AND 65535),
    health_path TEXT NOT NULL CHECK (health_path ~ '^/[A-Za-z0-9._~!$&''()*+,;=:@%/-]*$'),
    environment JSONB NOT NULL DEFAULT '{}'::jsonb,
    placement JSONB NOT NULL DEFAULT '{}'::jsonb,
    generation BIGINT NOT NULL DEFAULT 1 CHECK (generation > 0),
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    CHECK (name ~ '^[a-z][a-z0-9-]{0,62}$'),
    CHECK (length(image) BETWEEN 1 AND 512),
    CHECK (octet_length(environment::text) <= 16384),
    CHECK (octet_length(placement::text) <= 8192)
);

CREATE TABLE IF NOT EXISTS nodes (
    node_id UUID PRIMARY KEY,
    name VARCHAR(63) NOT NULL UNIQUE,
    instance_id UUID NOT NULL,
    status VARCHAR(16) NOT NULL CHECK (status IN ('ready', 'not_ready', 'draining')),
    cpu_capacity_millis INTEGER NOT NULL CHECK (cpu_capacity_millis > 0),
    memory_capacity_mb INTEGER NOT NULL CHECK (memory_capacity_mb > 0),
    labels JSONB NOT NULL DEFAULT '{}'::jsonb,
    docker_version TEXT NOT NULL DEFAULT '',
    agent_version TEXT NOT NULL,
    agent_epoch BIGINT NOT NULL DEFAULT 1,
    last_heartbeat TIMESTAMPTZ NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    CHECK (name ~ '^[a-z][a-z0-9-]{0,62}$'),
    CHECK (octet_length(labels::text) <= 8192)
);

CREATE TABLE IF NOT EXISTS allocations (
    allocation_id UUID PRIMARY KEY,
    service_id UUID NOT NULL REFERENCES services(service_id) ON DELETE CASCADE,
    replica INTEGER NOT NULL CHECK (replica >= 0),
    node_id UUID REFERENCES nodes(node_id),
    desired_state VARCHAR(16) NOT NULL CHECK (desired_state IN ('running', 'stopped')),
    observed_state VARCHAR(24) NOT NULL CHECK (observed_state IN ('pending', 'pulling', 'starting', 'running', 'unhealthy', 'stopping', 'stopped', 'failed', 'lost')),
    service_generation BIGINT NOT NULL,
    allocation_revision BIGINT NOT NULL DEFAULT 1,
    container_id TEXT,
    endpoint TEXT,
    restart_count INTEGER NOT NULL DEFAULT 0,
    last_error TEXT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (service_id, replica),
    CHECK (container_id IS NULL OR length(container_id) <= 128),
    CHECK (endpoint IS NULL OR length(endpoint) <= 512),
    CHECK (last_error IS NULL OR length(last_error) <= 2048)
);

CREATE TABLE IF NOT EXISTS commands (
    command_id UUID PRIMARY KEY,
    dedupe_key TEXT NOT NULL UNIQUE,
    allocation_id UUID NOT NULL REFERENCES allocations(allocation_id) ON DELETE CASCADE,
    node_id UUID NOT NULL REFERENCES nodes(node_id),
    kind VARCHAR(16) NOT NULL CHECK (kind IN ('ensure', 'stop', 'restart')),
    payload JSONB NOT NULL,
    service_generation BIGINT NOT NULL,
    allocation_revision BIGINT NOT NULL,
    status VARCHAR(16) NOT NULL CHECK (status IN ('pending', 'leased', 'succeeded', 'retry', 'dead')),
    lease_owner UUID,
    lease_token UUID,
    lease_expires_at TIMESTAMPTZ,
    available_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    attempt_count INTEGER NOT NULL DEFAULT 0,
    max_attempts INTEGER NOT NULL DEFAULT 8,
    last_error TEXT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    completed_at TIMESTAMPTZ,
    CHECK (octet_length(payload::text) <= 65536)
);

CREATE TABLE IF NOT EXISTS events (
    event_id BIGSERIAL PRIMARY KEY,
    event_type VARCHAR(96) NOT NULL,
    aggregate_id TEXT NOT NULL,
    message TEXT NOT NULL,
    payload JSONB NOT NULL DEFAULT '{}'::jsonb,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    CHECK (length(message) <= 2048),
    CHECK (octet_length(payload::text) <= 65536)
);

CREATE INDEX IF NOT EXISTS idx_nodes_health ON nodes(status, last_heartbeat);
CREATE INDEX IF NOT EXISTS idx_allocations_service ON allocations(service_id, replica);
CREATE INDEX IF NOT EXISTS idx_allocations_node_state ON allocations(node_id, observed_state);
CREATE INDEX IF NOT EXISTS idx_commands_claim ON commands(node_id, status, available_at, created_at);
CREATE INDEX IF NOT EXISTS idx_events_recent ON events(created_at DESC, event_id DESC);

INSERT INTO schema_migrations(version, description)
VALUES (1, 'initial durable desired state, scheduling, outbox, and audit schema')
ON CONFLICT (version) DO NOTHING;

COMMIT;
