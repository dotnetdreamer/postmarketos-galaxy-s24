# Modem: mobile data, SMS, calls

**State:**
- Mobile data works (LTE, 78 to 135 Mbit/s measured), with signal bars and the data switch in Plasma Mobile
- SMS: code done, checked over D-Bus, not yet tested with a real message
- Calls: dial, answer, hang up and DTMF work over D-Bus (GNOME Calls sees a normal modem); the in-call voice path is reverse engineered but not built, so a call has no sound yet

## Design

Samsung's modem processor (CP) is driven by Samsung's own Android RIL (`rild`) and boot daemon (`cbd`). They run in the Android container ([../ARCHITECTURE.md](../ARCHITECTURE.md)). On the Linux side, `s24-mmd` talks to rild over binder and pretends to be ModemManager:

```
NetworkManager, Plasma, GNOME Calls, mmcli
        │  D-Bus: org.freedesktop.ModemManager1 (Modem, Modem3gpp, Simple, SIM, Bearer, Messaging, Voice)
        ▼
s24-mmd (src/modem/s24-mmd.c, libgbinder + libmm-glib's generated D-Bus skeletons)
        │  AIDL: android.hardware.radio.{sim,modem,network,data,messaging,voice}/slotN
        ▼
rild + cbd (container) ── modem processor
```

The real ModemManager can't drive this modem; `rootfs/usr/local/share/dbus-1/system-services/org.freedesktop.ModemManager1.service` stops D-Bus from starting it.

## Files

| File | Job |
|---|---|
| `src/modem/s24-mmd.c` | The ModemManager stand-in |
| `src/modem/s24-radio.c` | A command-line radio tool (`status`, `power`, `data`, `net`, `usage`); refuses to run while `s24-mmd` runs (rild only answers its last client), `S24_RADIO_FORCE=1` overrides |
| `src/modem/aidlver.c` | Prints a binder service's AIDL version and hash: check it before trusting method numbers |
| `src/modem/s24selinux.c` | `LD_PRELOAD` shim: the container's service managers insist on an SELinux status page |
| `src/modem/gbinder.conf` | `/etc/gbinder.conf`: `aidl3` for protocol and service manager |
| `rootfs/usr/local/sbin/android-env.sh`, `android-ctr0.sh`, `android-ctr.sh` | Build and start the container |
| `rootfs/usr/local/share/s24/init.s24.rc`, `cgroup.rc`, `apex-info-list.xml` | The container's only init script, a hand-written cgroup description, the APEX list for its linker config |
| `rootfs/usr/local/sbin/s24-mmd-run`, `rootfs/etc/init.d/s24-mmd` | Boot service: starts the container if needed, then `s24-mmd` |
| `rootfs/usr/local/sbin/s24-mobile-data-hook`, `rootfs/etc/NetworkManager/dispatcher.d/90-s24-mobile-data` | DNS fallback and CLAT (IPv4 on IPv6-only networks) |
| `src/callaudio/` | Call audio: what Samsung's audio HAL does around a call (mixer route, modem audio commands), mapped from its libraries; `s24-callaudio.py` sets the Linux side |

`s24-mobile-data` (the older data-only service) is kept but not enabled.

## Things that took a long time

- **2G only?** Samsung's RIL ignores the initial attach APN when it doesn't know the SIM operator. Android sets the property `gsm.sim.operator.numeric` first (from the IMSI). `s24-mmd` does the same: card status, IMSI, set the property, attach APN, data allowed, radio on, wait for LTE, data call
- The APN is chosen from the stock `apns-conf.xml` by MCC/MNC (MVNO IMSI patterns win)
- NetworkManager: a modem that turns "disabled" while connected counts as "disconnected by user" and never reconnects. `s24-mmd` removes the modem from D-Bus while rild is gone and adds it back after a real SIM status, so NM sees a new device and connects
- postmarketOS's NetworkManager policy puts mobile routes in table 1430; the hook adds an `ip rule` so it works as a fallback when Wi-Fi is gone
- IPv6-only carriers: clatd + TAYGA give IPv4; the CLAT address is pinned to the mobile prefix (the default "shared" mode took Wi-Fi's address and needs nftables, which this kernel lacks)
- rild crashes when given binders that are already dead, so `s24-mmd` registers with rild only after it owns the D-Bus name
- A rild that restarted while the modem kept running answers only "Invalid response: NULL": restart the modem processor (`setprop ctl.restart` on cbd in the container)
- Every real radio power change resets the PC's USB link once
- Dual-SIM properties must be set (`ro.vendor.multisim.simslotcount=2`, `persist.radio.multisim.config=dsds`) or rild opens only the first channel
- rild waits for `android.system.suspend.ISystemSuspend/default`: the container runs `android.system.suspend-service`

## Test

```sh
mmcli -L; mmcli -m 0; mmcli -b 0
nmcli -f GENERAL,IP4,IP6 device show rmnet0
```

`tests/modem/`: data, DNS64, Wi-Fi-off fallback, NetworkManager toggling, an SMS draft (never sent).
