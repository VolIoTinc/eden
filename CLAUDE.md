# Eden — working rules for AI assistants

## THIS REPOSITORY IS PUBLIC (github.com/VolIoTinc/eden)

Everything committed here is world-readable. Act accordingly at all times, not just when asked:

- **Never commit secrets**: WiFi SSIDs or passwords, MQTT/ntfy/Grafana/OTA passwords, API keys,
  Tailscale IPs (100.x.y.z), WAN/LTE addresses, MAC addresses, device serial numbers.
- Secrets live only in gitignored places: `.secrets/` (dev box), `projects/*/firmware/data/config.json`
  (node config), `/etc/eden/*` (on the Pi). Templates use obvious placeholders (`YOUR_...`, `FILL_ME_IN`).
- **Commit messages and docs are public too.** No home location, street, city, or personal details.
  Private LAN addresses (10.0.0.x, 192.168.x.x) and hostnames are acceptable; nothing routable is.
- **Tool output can leak**: uploaders, debug logs and `cat` of config files may print secrets. Filter
  before showing (`sed 's/--auth=[^ ]*/<redacted>/'`, `grep -v`), and never paste raw output into
  files, commits, issues or chat. If a secret does get printed, rotate it.
- Before any `git push`, run the pre-commit audit manually on the full range:
  `.githooks/pre-commit --range origin/main..HEAD`.
- When in doubt whether something is sensitive, treat it as sensitive and ask Andy.

Enable the hook once per clone: `git config core.hooksPath .githooks`.

## Project shape

- `docs/mqtt-contract.md` is the source of truth for every topic and payload.
- `firmware/lib/logic/` is pure, host-tested C++ shared by every node (`pio test -e native`).
- `projects/<plant>/` holds one plant's hardware, firmware config, care log and data.
- `pi/` provisions Serenity (the shared Pi 4B gateway). It never touches iptables, dnsmasq,
  NetworkManager, Tailscale or Greengrass. Snapshot + diff the network state around any install.
- No Docker on the Pi.
