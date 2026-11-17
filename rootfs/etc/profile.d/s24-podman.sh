# /etc/profile.d/s24-podman.sh (Claude, 2026-11-15): podman for the phone's user goes through the
# root API service (s24-podman-api), since this kernel has no user namespaces for rootless podman.
if [ "$(id -u)" != 0 ] && [ -w /run/podman/podman.sock ]; then
	export CONTAINER_HOST=unix:///run/podman/podman.sock
fi
