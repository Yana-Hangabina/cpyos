# Security policy

Report vulnerabilities through GitHub's **Security → Report a vulnerability** flow. Do not open a public issue for account data, credentials, a live JJWXC/WeRead session, or an unfixed security problem.

The project does not accept or store accounts, passwords, tokens, cookies, device verification codes, or copyrighted chapter text. Serial logs pasted into issues must be redacted: remove `loginName=`, `loginPassword=`, `sid=`, `Cookie:`, `Authorization:` and any reader id.

See [docs/cpyos/security.md](docs/cpyos/security.md) for what the firmware stores, how it is protected, and the current TLS limitations.

Supported versions: the latest CPYOS release. Older releases may be asked to reproduce on the current tag before a fix is backported.
