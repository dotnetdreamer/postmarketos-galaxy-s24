# real.sh: sourced. The real desktop session's addresses, from its plasmashell (the one in the seat's
# session cgroup, never a test copy), for read-only looks and for starting s24-remote as the session would.
for p in $(pgrep -x plasmashell); do
    grep -q "^0::/c[0-9]*$" /proc/$p/cgroup 2>/dev/null || continue
    eval "$(tr '\0' '\n' </proc/$p/environ | grep -E '^(DBUS_SESSION_BUS_ADDRESS|WAYLAND_DISPLAY|XDG_RUNTIME_DIR|XDG_SESSION_ID|PLASMA_PLATFORM|XDG_CONFIG_DIRS|XDG_CURRENT_DESKTOP|DISPLAY)=' | sed 's/^/export /; s/=/="/; s/$/"/')"
    REAL_PLASMASHELL=$p
    REAL_CGROUP=$(sed -n 's/^0:://p' /proc/$p/cgroup)
    break
done
