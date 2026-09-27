BEGIN;

-- Sequence values are durable, monotonic, and deliberately non-transactional:
-- an aborted controller startup may leave a gap but can never reuse an epoch.
CREATE SEQUENCE IF NOT EXISTS controller_epoch_sequence
    AS BIGINT
    MINVALUE 1
    START WITH 1
    NO CYCLE;

INSERT INTO schema_migrations(version, description)
VALUES (2, 'durable monotonic controller epochs for restart fencing')
ON CONFLICT (version) DO NOTHING;

COMMIT;
