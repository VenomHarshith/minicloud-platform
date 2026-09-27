BEGIN;

-- Older prerelease schemas accepted a trailing hyphen even though worker and
-- allocation identities use the stricter portable DNS-label subset. Fail with
-- a clear diagnostic instead of silently renaming durable identities.
DO $$
BEGIN
    IF EXISTS (
        SELECT 1 FROM services
        WHERE name !~ '^[a-z]([a-z0-9-]{0,61}[a-z0-9])?$'
    ) THEN
        RAISE EXCEPTION
            'services contains a name outside the MiniCloud DNS-label contract';
    END IF;
    IF EXISTS (
        SELECT 1 FROM nodes
        WHERE name !~ '^[a-z]([a-z0-9-]{0,61}[a-z0-9])?$'
    ) THEN
        RAISE EXCEPTION
            'nodes contains a name outside the MiniCloud DNS-label contract';
    END IF;
END
$$;

ALTER TABLE services DROP CONSTRAINT IF EXISTS services_name_check;
ALTER TABLE services DROP CONSTRAINT IF EXISTS services_name_dns_label;
ALTER TABLE services ADD CONSTRAINT services_name_dns_label
    CHECK (name ~ '^[a-z]([a-z0-9-]{0,61}[a-z0-9])?$');

ALTER TABLE nodes DROP CONSTRAINT IF EXISTS nodes_name_check;
ALTER TABLE nodes DROP CONSTRAINT IF EXISTS nodes_name_dns_label;
ALTER TABLE nodes ADD CONSTRAINT nodes_name_dns_label
    CHECK (name ~ '^[a-z]([a-z0-9-]{0,61}[a-z0-9])?$');

INSERT INTO schema_migrations(version, description)
VALUES (4, 'consistent DNS-label constraints for services and nodes')
ON CONFLICT (version) DO NOTHING;

COMMIT;
