/* Galaxy S24 postmarketOS (Claude, 2026-11-15): s24bt = the whole Bluetooth stack in ONE module
 * that exports nothing. Samsung's kernel stubs out the HCI socket (net/bluetooth/hci_sock.c: on
 * Android Bluetooth goes through their HAL), and GKI's MODULE_SIG_PROTECT rejects any unsigned
 * module that exports one of bluetooth.ko's (protected) symbols. Linked together, the core
 * (with hci_sock restored), RFCOMM and hci_uart need no exports at all.
 * Force-included into every object of the module (ccflags -include). */
#ifndef S24BT_COMPAT_H
#define S24BT_COMPAT_H
#include <linux/init.h>
#include <linux/export.h>
/* the parts' exit code is called from the shared init's error path and exit: keep it in .text */
#undef __exit
#define __exit __cold notrace
#undef EXPORT_SYMBOL
#undef EXPORT_SYMBOL_GPL
#undef EXPORT_SYMBOL_NS
#undef EXPORT_SYMBOL_NS_GPL
#define EXPORT_SYMBOL(sym)
#define EXPORT_SYMBOL_GPL(sym)
#define EXPORT_SYMBOL_NS(sym, ns)
#define EXPORT_SYMBOL_NS_GPL(sym, ns)
/* hci_uart: H4 and Qualcomm's protocol (H4 plus in-band sleep, which the WCN7851 firmware always
 * uses); s24-bt loads the firmware from userspace and attaches N_HCI + QCA. No serdev, no
 * LL/BCM (they would need other modules) */
#undef CONFIG_BT_HCIUART_SERDEV
#undef CONFIG_BT_HCIUART_LL
#undef CONFIG_BT_HCIUART_BCM
#define CONFIG_BT_HCIUART_H4 1
#endif
