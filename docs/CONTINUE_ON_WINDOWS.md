# Continue MiniCloud from a new Windows laptop

This guide starts with a new Windows laptop and ends with a tested MiniCloud
change pushed to GitHub. It is written for the repository owner, but the
feature-branch workflow also works for collaborators who have write access.

Repository: <https://github.com/VenomHarshith/minicloud-platform>

The normal authentication path is HTTPS with Git Credential Manager (GCM).
Each laptop signs in as a GitHub user; it does **not** need a repository deploy
key, a copied SSH private key, or a personal access token pasted into a URL.
The HTTPS remote includes the intended GitHub username so GCM keeps this
personal identity separate from another GitHub account on the same laptop.

## First understand what moves between laptops

| Item | Stored in GitHub? | What to do on the new laptop |
|---|---:|---|
| Source, tests, scripts, and guides | Yes | Clone or pull the repository. |
| Git commit history and pushed branches | Yes | Fetch them from `origin`. |
| `deploy/.env` credentials | No | Generate new random values locally. |
| PostgreSQL data and Prometheus history | No | Start with new local Docker volumes. |
| Built images and running containers | No | Build/start them again with the checked-in scripts. |
| A Cloudflare Quick Tunnel URL | No durable URL | Start and verify a new temporary tunnel when needed. |

Pushing a branch makes code available on another laptop. It does not migrate a
running cluster, database, local image, secret, or temporary public URL.

## 1. Install the prerequisites

Use Windows 11 with current updates. Install:

1. **Git for Windows** from <https://git-scm.com/download/win>. Keep the Git
   Credential Manager component enabled in the installer.
2. **Docker Desktop for Windows** using its WSL 2 backend. Follow Docker's
   [Windows installation guide](https://docs.docker.com/desktop/setup/install/windows-install/)
   and [WSL 2 workflow](https://docs.docker.com/desktop/features/wsl/use-wsl/).
3. **Visual Studio Code**, optionally, from <https://code.visualstudio.com/>.
   The repository recommends its C++, CMake, Docker, and YAML extensions when
   opened.

If WSL is not installed, open PowerShell as Administrator and run:

```powershell
wsl --install
```

Restart if Windows requests it, open Docker Desktop, enable the WSL 2 engine,
and use **Linux containers**. MiniCloud's supported Windows full-stack path is
Linux containers inside Docker Desktop, not Windows containers.

The project itself uses only free/open-source dependencies and does not require
a paid cloud service. Under Docker's current terms, Docker Desktop is free for
personal use and education; commercial use in a larger organization can require
a paid subscription. Review the current terms before using it for employer
work. No payment is required for this personal learning workflow.

Open a new ordinary PowerShell window and verify:

```powershell
git --version
docker version
docker compose version
docker info --format '{{.OSType}}'
```

The last command must print `linux`. If the Docker commands cannot connect,
wait for Docker Desktop to finish starting. Give Docker at least 6 GiB of
memory for the first full build when the host has enough RAM.

## 2. Prepare the correct GitHub identity

The repository owner should use the personal GitHub account
**VenomHarshith**, not an employer account. A collaborator should use their own
GitHub account after the owner grants it access; nobody should share the
owner's credentials. Open <https://github.com/settings/emails>, enable the
desired email-privacy setting, and copy the exact GitHub-provided
`users.noreply.github.com` address.
GitHub documents this at
[Setting your commit email address](https://docs.github.com/en/account-and-profile/how-tos/email-preferences/setting-your-commit-email-address).

The next section sets identity only inside this repository. That is safer than
changing global identity on a laptop that may also hold unrelated repositories.

Git commit identity and GitHub authentication are different:

- `user.name` and `user.email` label a commit.
- GCM's browser sign-in decides which GitHub account is allowed to push.
- Signing into VS Code Settings Sync does not by itself grant Git push access.

## 3. Clone through HTTPS

Choose a development folder that you own. This example uses `C:\dev`:

```powershell
New-Item -ItemType Directory -Force C:\dev | Out-Null
Set-Location C:\dev
git clone https://VenomHarshith@github.com/VenomHarshith/minicloud-platform.git
Set-Location .\minicloud-platform
```

`VenomHarshith` before `@github.com` selects the owner's personal GCM identity;
it is a username, not a secret. A collaborator replaces only that first
username with their own authorized GitHub login while leaving the
`VenomHarshith/minicloud-platform.git` repository path unchanged. This follows
GCM's official
[multiple-user guidance](https://github.com/git-ecosystem/git-credential-manager/blob/main/docs/multiple-users.md).

GitHub's official cloning procedure is documented in
[Cloning a repository](https://docs.github.com/en/repositories/creating-and-managing-repositories/cloning-a-repository).
Because this repository is public, cloning may not ask you to sign in. The
first push is normally when GCM opens GitHub in the browser.

Confirm that this is the intended repository and that the checkout is clean:

```powershell
git remote get-url origin
git status
git log -1 --oneline
```

The remote must be exactly:

```text
https://VenomHarshith@github.com/VenomHarshith/minicloud-platform.git
```

For a collaborator, the username before `@github.com` is different as described
above; the repository owner/path after the host is still `VenomHarshith`.

Configure the local commit identity using the exact name and no-reply address
you chose in GitHub settings:

```powershell
git config --local user.name "YOUR NAME"
git config --local user.email "COPY_YOUR_EXACT_GITHUB_NOREPLY_EMAIL"
git config --local --get user.name
git config --local --get user.email
```

Do not type the placeholder values literally. The email must be an address
shown by the personal GitHub account if you want commits attributed correctly.

### What happens on the first push

Git for Windows normally uses GCM. On the first authenticated operation, GCM
opens a browser-based GitHub sign-in/authorization flow and stores the resulting
credential in Windows Credential Manager. GitHub's current instructions are in
[Caching your GitHub credentials](https://docs.github.com/en/get-started/git-basics/caching-your-github-credentials-in-git).

At the browser prompt:

1. Verify that the site is `github.com`.
2. If you are the owner, sign in as **VenomHarshith**. A collaborator signs in
   with their own authorized account.
3. Complete any two-factor-authentication prompt yourself.
4. Approve Git Credential Manager only if the page shows the expected account.

Do not send a verification code to another person or paste it into project
files. Do not put a GitHub password, token, or private key in the clone URL.
GitHub no longer accepts an account password for Git HTTPS operations.

## 4. Open the project in VS Code

From the repository root:

```powershell
code .
```

If `code` is not recognized, use **File → Open Folder** in VS Code and select
`C:\dev\minicloud-platform`, or reinstall VS Code with its command-line option
enabled.

Trust the folder only after confirming the remote above. Accept the repository's
recommended extensions if useful. Use **Terminal → Run Task** to see the
checked-in MiniCloud tasks. The complete task mapping is in [VSCODE.md](VSCODE.md).

The Source Control view can stage, commit, publish a branch, and synchronize.
The terminal commands in this guide remain the clearest way to confirm exactly
which branch, remote, and files are involved.

## 5. Create fresh local credentials—never copy old secrets

Every laptop should have its own ignored `deploy/.env`. Do not download this
file from another laptop, put it in chat/email/cloud storage, or commit it.

From PowerShell in the repository root:

```powershell
Test-Path .\deploy\.env
.\deploy\powershell\Initialize-MiniCloud.ps1
git check-ignore .\deploy\.env
git status --short
```

For a new clone, `Test-Path` should initially print `False`. The initialization
script creates three random local secrets and refuses to overwrite an existing
file. `git check-ignore` should print `deploy/.env`, and `git status --short`
must not list it.

Keep the generated file while using that laptop's existing MiniCloud database
volume. Replacing its database password does not change a password already
stored in PostgreSQL. If the file is lost, stop: GitHub has no recoverable copy.
If the local data matters, recover the original file from an approved private
backup or perform a deliberate PostgreSQL credential-recovery procedure before
restarting. If the local data is disposable, first read the volume-destruction
warning in [OPERATIONS.md](OPERATIONS.md), intentionally remove the old local
volumes, and only then initialize a new file. Never generate a new database
password while reusing the old PostgreSQL volume.

## 6. Build and prove the project on this laptop

Validate the Compose model first:

```powershell
docker compose --env-file deploy/.env `
  -f deploy/compose.yaml `
  -f deploy/compose.desktop.yaml `
  config --quiet
```

Then start MiniCloud and deploy the real two-replica echo workload:

```powershell
.\scripts\windows\Start-MiniCloud.ps1
.\scripts\windows\Deploy-Echo.ps1
```

The first build downloads public images and build dependencies, so it takes
longer than later cached builds. A successful workload proof ends with:

```text
MiniCloud end-to-end verification passed.
```

Open:

- Dashboard: <http://127.0.0.1:3000>
- Workload gateway: <http://127.0.0.1:8080>
- Controller API: <http://127.0.0.1:8090>
- Prometheus: <http://127.0.0.1:9090>

Stop cleanly without deleting PostgreSQL or Prometheus volumes:

```powershell
.\scripts\windows\Stop-MiniCloud.ps1
```

Do not add `--volumes` unless you intentionally want to delete local runtime
state. See [RUN_INSTRUCTIONS.md](../RUN_INSTRUCTIONS.md) and
[WINDOWS.md](WINDOWS.md) for the full operating and troubleshooting reference.

## 7. Use a feature branch for every change

Start each work session by checking the worktree before switching branches:

```powershell
git status
```

Stop here unless the worktree is clean. Finish and commit the current work, or
save it deliberately on its existing branch, before continuing. Never discard
unfamiliar changes. Then update `main`:

```powershell
git switch main
git fetch --prune origin
git pull --ff-only origin main
```

Create a short, descriptive branch:

```powershell
git switch -c feature/describe-the-change
```

Edit in VS Code. Then review everything before staging:

```powershell
git status --short
git diff
git diff --check
```

Run checks proportionate to the change. Useful Windows checks include:

```powershell
docker compose --env-file deploy/.env `
  -f deploy/compose.yaml `
  -f deploy/compose.desktop.yaml `
  config --quiet
```

For a full platform or C++/runtime change:

```powershell
.\scripts\windows\Start-MiniCloud.ps1
.\scripts\windows\Deploy-Echo.ps1
```

For a documentation or repository-contract change, use the Windows Python
launcher if it is installed:

```powershell
py -3 .\scripts\check_repository.py
```

If `py` is unavailable but `python --version` reports Python 3, use:

```powershell
python .\scripts\check_repository.py
```

GitHub Actions also runs the complete repository, C++, dashboard, Compose,
Windows, and container end-to-end checks after a push.

Stage only intended files; avoid `git add .` until you are comfortable auditing
every untracked file:

```powershell
git add path\to\changed-file
git status --short
git diff --cached
git diff --cached --check
git commit -m "Explain the change briefly"
```

Push the feature branch:

```powershell
git push -u origin feature/describe-the-change
```

The first push may open the GCM browser login described earlier. After the push,
open the GitHub repository, select **Compare & pull request**, review the diff,
wait for CI, and merge when the checks pass. GitHub explains the remote update
commands in
[Getting changes from a remote repository](https://docs.github.com/en/get-started/using-git/getting-changes-from-a-remote-repository).

After the pull request is merged:

```powershell
git switch main
git pull --ff-only origin main
git branch -d feature/describe-the-change
```

Deleting the local feature branch does not delete the merged commits.

### Direct pushes to `main`

The repository owner may currently be able to push directly to `main`, but a
feature branch and pull request are safer: they show the complete diff, run CI,
and provide a review/rollback boundary. If a small urgent direct push is truly
intentional, update `main`, review the staged diff, and run the checks first.
Never use `--force` on `main`.

## 8. Keep two laptops synchronized

At the **start** of work on either laptop, first run:

```powershell
git status
```

Continue only when the worktree is clean. Then update `main` and start the new
branch:

```powershell
git switch main
git fetch --prune origin
git pull --ff-only origin main
git switch -c feature/new-work
```

At the **end** of work, commit and push the feature branch. An unpushed commit
exists only on that laptop.

To continue an already-pushed feature branch on the other laptop:

```powershell
git fetch --prune origin
git switch --track origin/feature/describe-the-change
```

If that branch already exists locally:

```powershell
git switch feature/describe-the-change
git pull --ff-only origin feature/describe-the-change
```

Do not work on the same unpushed branch on two laptops at once. If `--ff-only`
refuses because histories diverged, stop and inspect:

```powershell
git status
git log --oneline --graph --decorate --all -20
```

Preserve both sets of work on named branches and merge deliberately. Do not fix
divergence with `git push --force`, `git reset --hard`, or by deleting a working
folder.

## 9. Never commit secrets or machine-local files

Before every commit and push, inspect both lists:

```powershell
git status --short
git diff --cached
```

Never commit:

- `deploy/.env` or another real `.env` file;
- GitHub tokens, passwords, two-factor codes, deploy keys, or private SSH keys;
- Docker registry credentials or Docker's `config.json`;
- database dumps, local volumes, logs, browser cookies, or support archives;
- employer/customer source, infrastructure names, screenshots, or production
  data;
- generated `build/`, `dist/`, `dashboard/node_modules/`, or dashboard build
  output.

The repository's `.gitignore`, package scripts, and contract check add layers
of protection, but they do not replace reviewing the staged diff.

If `deploy/.env` is accidentally staged but not committed, unstage it without
deleting the local file:

```powershell
git restore --staged deploy/.env
git check-ignore deploy/.env
```

If any credential reaches a commit or remote, assume it is compromised. Stop
sharing it, rotate/revoke it first, and follow GitHub's
[sensitive-data removal guidance](https://docs.github.com/en/authentication/keeping-your-account-and-data-secure/removing-sensitive-data-from-a-repository).
Deleting the file in a later commit does not erase it from earlier history.

## 10. Preserve repository line endings

MiniCloud's checked-in `.gitattributes` is authoritative:

- the listed C++, protocol, CMake, Markdown, JSON, YAML, shell, and Dockerfile
  patterns are explicitly checked out with LF;
- PowerShell `.ps1` files use CRLF.

Other text files use Git's automatic text normalization, so their working-tree
ending can depend on Git settings even though committed text is normalized.

Git for Windows and VS Code should honor those rules. Do not run a repository-wide
line-ending conversion or `git add --renormalize .` as part of an unrelated
change. If a one-line edit appears as a whole-file diff, stop and correct the
editor's line-ending mode before committing.

Check for whitespace/line-ending damage with:

```powershell
git diff --check
git diff --cached --check
```

When cloning inside WSL instead of `C:\dev`, use Linux Git and VS Code's WSL
extension consistently for that checkout. Avoid editing one checkout alternately
through Windows Git and WSL Git with conflicting global settings.

## 11. Authentication and push troubleshooting

### The browser opens the wrong GitHub account

Cancel instead of approving. Sign out of the wrong GitHub browser profile or
open the personal profile, then confirm `git remote get-url origin` includes
the intended account immediately before `@github.com`. If Windows still
supplies an incorrect account, open **Credential Manager → Windows
Credentials** and remove only the GitHub credential whose target/user is the
intended personal identity, such as `VenomHarshith@github.com`. Do not remove a
generic or employer GitHub credential. The next push for this account should
start a fresh GCM browser login.

### Git asks for a password in the terminal

Do not enter the GitHub account password. Update/reinstall current Git for
Windows with Git Credential Manager enabled, close old terminals, and retry.
The supported flow is browser authentication through GCM.

### Push returns `403` or permission denied

Check all three items:

```powershell
git remote get-url origin
git config --local --get user.email
git status --branch --short
```

The remote path should point to `VenomHarshith/minicloud-platform`, the username
before `@github.com` should be the intended personal account, and GCM must be
authenticated as an account with write access. Commit email alone does not
grant access. A contributor without write permission should fork the repository
and open a pull request instead.

### Push is rejected because the remote contains new work

Do not force push. On a feature branch, fetch and inspect the copy of that same
feature branch on GitHub. Replace the example branch name with your branch:

```powershell
git fetch --prune origin
git status --branch --short
git log --oneline --graph --decorate HEAD origin/feature/describe-the-change -20
git pull --ff-only origin feature/describe-the-change
git push
```

If `--ff-only` refuses because both copies have new commits, preserve both
histories. Review the graph, then deliberately merge the remote feature branch,
resolve only the marked conflicts, rerun tests, and push:

```powershell
git merge origin/feature/describe-the-change
git push
```

Bringing `origin/main` into the feature branch is a separate step used to make
the pull request current; it does not by itself recover commits that another
laptop already pushed to the feature branch.

If the rejected branch is `main`, switch to a feature branch and open a pull
request instead of rewriting public history.

### Docker reports Windows containers

Switch Docker Desktop to Linux containers, then confirm:

```powershell
docker info --format '{{.OSType}}'
```

### PowerShell blocks a reviewed project script

Use a process-only exception, not a permanent machine-wide weakening:

```powershell
Set-ExecutionPolicy -Scope Process Bypass
```

Review the script first, then run it from the repository root.

### `deploy/.env` already exists

The initializer refuses to overwrite credentials intentionally. If this is an
existing working checkout, preserve the file. If this is supposed to be a
brand-new clone, investigate why the file exists before changing it; never
replace it while reusing an initialized database volume.

## 12. Quick start checklist for every additional Windows laptop

1. Install current Git for Windows with GCM, Docker Desktop/WSL 2, and optionally
   VS Code.
2. Confirm Docker reports `linux` containers.
3. Clone the HTTPS repository URL.
4. Set repository-local name and the exact GitHub no-reply email.
5. Generate a new ignored `deploy/.env`; never transfer the old one.
6. Validate Compose, start MiniCloud, and run `Deploy-Echo.ps1`.
7. Pull `main`, create a feature branch, edit, test, review, commit, and push.
8. Authenticate GCM as **VenomHarshith** when you are the owner, or as your own
   authorized GitHub account when collaborating.
9. Use a pull request and wait for CI.
10. Pull the merged `main` on every other laptop before starting new work.

Removing a temporary repository deploy key does not change this workflow. Each
Windows laptop receives access through the personal GitHub account's GCM login,
and that access can be revoked independently from Windows Credential Manager or
GitHub account settings.
