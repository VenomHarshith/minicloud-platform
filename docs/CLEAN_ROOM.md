# Clean-room contribution policy

MiniCloud must remain independently understandable and safe to publish.

Allowed:

- original implementation and diagrams;
- synthetic names, data, incidents, and workloads;
- public standards and official product documentation;
- dependencies reviewed in `docs/DEPENDENCIES.md`.

Forbidden:

- employer or customer source code;
- internal architecture, configurations, tickets, screenshots, or logs;
- private endpoints, repository names, credentials, certificates, or tokens;
- production/user data;
- material copied from non-public products, training, or documentation.

Before a commit or archive, inspect tracked files, search for credentials and
absolute personal paths, verify `deploy/.env` is ignored, and build from a clean
checkout. Examples must use reserved/synthetic names only.
