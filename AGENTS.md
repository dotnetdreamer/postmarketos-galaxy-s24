# Notes for AI coding agents

You are working on a postmarketOS port for the Galaxy S24 SM-S921B (Exynos 2400) that runs on Samsung's stock kernel. Before you touch the phone:

1. Read [docs/LESSONS.md](docs/LESSONS.md), especially "Never do this": several ordinary-looking commands panic this kernel or destroy irreplaceable data (EFS)
2. Read [docs/DEVELOPING.md](docs/DEVELOPING.md): how to reach the phone, the habits that avoid trouble (wake locks, `unset LD_PRELOAD`, anchored `pgrep`), how to test without disturbing the person using the phone
3. The per-part pages in [docs/hardware/](docs/hardware/) record what was tried, what failed and why. Check them before retrying an idea

Ground rules:

- The phone may be in someone's hand. Never inject taps or key presses into a phone that may be in use or locked, and never leave a test window on its screen
- Ask before flashing anything, rebooting, or touching partitions; never install newer stock firmware
- Keep secrets out of this repo: no passwords, Wi-Fi details, IP addresses of private networks, IMEI, ICCID, IMSI, phone numbers, Bluetooth or MAC addresses, EFS contents, logs that contain any of these, and no Samsung binaries
- When you change something on the phone, bring the change back into `rootfs/` or `src/` here, and add what you learned to the right doc
