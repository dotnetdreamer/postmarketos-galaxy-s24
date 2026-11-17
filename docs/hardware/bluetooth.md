# Bluetooth

**State:** works with BlueZ: the controller comes up at boot, scanning finds devices, it survives deep sleep. Pairing and audio with real headphones not tested yet.

## Hardware

- Qualcomm WCN7851 (the Bluetooth half of the Wi-Fi combo chip) on `/dev/ttySAC1`
- Powered through Samsung's `btpower` driver (`/dev/btpower`, ioctl `0xbfad`)
- Firmware `/vendor/firmware/hmtbtfw20.tlv` + `hmtnv20.b20`
- The address is in EFS (`bluetooth/bt_addr`): mount EFS read-only once and copy it to `/etc/s24/bt_addr` (keep that file private)

## Why a custom kernel module

Samsung's kernel ships the Bluetooth core with `hci_sock.c` stubbed out (Android talks to the chip through its own HAL), so BlueZ panics the kernel. A rebuilt `bluetooth.ko` can't load either: it's a protected GKI module, and an unsigned module may not export its symbols.

`s24bt.ko` links the Bluetooth core (with `hci_sock` restored), RFCOMM, `hci_uart` (H4 + Qualcomm protocol) and `btqca` into **one module that exports nothing**. Sources come from Samsung's own kernel tree; this repo has the glue:

| File | Job |
|---|---|
| `src/bluetooth/s24bt/prepare.sh` | Copies the sources out of the kernel tree and renames the parts' init functions |
| `src/bluetooth/s24bt/patch-qca.sh` | Makes `hci_qca` work as a line discipline (no serdev) at 3 Mbaud |
| `src/bluetooth/s24bt/s24bt_compat.h` | Forced include: no exports, no `__exit` section |
| `src/bluetooth/s24bt/s24bt_main.c` | One init that calls the parts in order |
| `src/bluetooth/s24bt/Kbuild` | The module |
| `src/bluetooth/s24bt/cmp_layouts.py` | Compares every struct in the running kernel's BTF with the module's DWARF (pahole) |

## Building s24bt.ko (in the container)

1. Get Samsung's kernel source for this firmware (6.1.93, the `android14-6.1` tree for SM-S921B, for example from opensource.samsung.com or a public mirror of it) into `/work/ksrc`
2. `apk add clang lld llvm gcc pahole make bison flex perl openssl-dev elfutils-dev linux-headers`
3. Use the phone's own config: `/proc/config.gz` from the phone as `.config`; only turn off `WERROR`. Set `.scmversion` to `-android14-11` with `LOCALVERSION=""` and `LOCALVERSION_AUTO=y`, so the release string is exactly `6.1.93-android14-11`
4. `make ARCH=arm64 LLVM=1 modules_prepare`, then `sh prepare.sh`, `sh patch-qca.sh`
5. `make -C /work/ksrc ARCH=arm64 LLVM=1 M=/work/s24bt KBUILD_MODPOST_WARN=1 modules`
6. Module CRCs: there's no `Module.symvers` for this kernel. Make one from the `__versions` sections of the phone's own modules (`llvm-objcopy --dump-section __versions=...`; 64-byte records: u64 CRC + 56-byte name)

**Before loading it, check** (a wrong module crashes the phone):

- `.gnu.linkonce.this_module` is 0x440 bytes (the size of this kernel's `struct module`)
- no `__ksymtab` sections, and the same vermagic as the phone's own modules (`modinfo`)
- every imported symbol exists in `/proc/kallsyms`
- `cmp_layouts.py` reports 0 differing structs (copy `/sys/kernel/btf/vmlinux` from the phone first)

Install it as `/usr/local/lib/s24/s24bt.ko`, next to Samsung's `btpower.ko` (from the vendor_boot or vendor_dlkm modules).

## Userspace

- `src/bluetooth/s24-bt.c`: powers the chip, follows upstream `btqca` for the WCN7850 family (version, 3 Mbaud, TLV rampatch, NVM), sets the address, then attaches the line with `HCI_UART_QCA` (protocol 8) and stays running. `S24BT_PROBE=1` dumps the in-band sleep bytes; `S24BT_NODEEPSLEEP=1` keeps the chip awake
- `rootfs/etc/init.d/s24-bluetooth`: loads both modules, does one warm-up start (the first start after loading `btpower` holds a wakeup source forever), then supervises `s24-bt` before `bluetooth`

`s24bt` can't be unloaded: a new build needs a reboot.
