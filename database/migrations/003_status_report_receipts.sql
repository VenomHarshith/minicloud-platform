BEGIN;

CREATE TABLE IF NOT EXISTS status_report_receipts (
    report_id UUID PRIMARY KEY,
    fingerprint TEXT NOT NULL,
    disposition VARCHAR(16) NOT NULL
        CHECK (disposition IN ('processing', 'accepted', 'stale')),
    service_name VARCHAR(63),
    expires_at TIMESTAMPTZ NOT NULL DEFAULT now() + interval '24 hours',
    CHECK (octet_length(fingerprint) <= 8192)
);

CREATE INDEX IF NOT EXISTS idx_status_report_receipts_expiry
    ON status_report_receipts(expires_at);

INSERT INTO schema_migrations(version, description)
VALUES (3, 'bounded durable workload-status idempotency receipts')
ON CONFLICT (version) DO NOTHING;

COMMIT;
