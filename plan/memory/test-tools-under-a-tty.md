---
name: test-tools-under-a-tty
description: Scripts the user runs must be tested under a pty — `timeout novacom` hangs (SIGTTIN) in an interactive terminal but not in the agent's shell
metadata:
  type: feedback
---
tools/grab.sh worked for me all session and hung the moment the user ran it: `timeout` puts novacom in a background process group, and from a real terminal novacom run/put touched the tty, got SIGTTIN, and sat stopped forever. The agent's Bash has no tty, so it never showed.

**Why:** the user is the one who runs these tools; a tool only proven headless is unproven. (2026-09-16)

**How to apply:** use `timeout --foreground N novacom … < /dev/null` in every device script, and before telling the user to run a tool, test it with `script -qec "<cmd>" /dev/null`.
