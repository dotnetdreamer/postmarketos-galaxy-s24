#!/bin/sh
# copy the Bluetooth sources out of the kernel tree into /work/s24bt and rename the parts' inits
set -e
K=/work/ksrc; D=/work/s24bt
rm -rf $D/bt $D/rfcomm $D/uart
mkdir -p $D/bt $D/rfcomm $D/uart
cp $K/net/bluetooth/*.c $K/net/bluetooth/*.h $D/bt/
cp $K/net/bluetooth/rfcomm/core.c $K/net/bluetooth/rfcomm/sock.c $K/net/bluetooth/rfcomm/tty.c $D/rfcomm/
cp $K/drivers/bluetooth/hci_ldisc.c $K/drivers/bluetooth/hci_h4.c $K/drivers/bluetooth/*.h $D/uart/
sed -i -e 's/^static int __init bt_init(void)/int __init s24bt_core_init(void)/' \
	-e 's/^static void __exit bt_exit(void)/void __exit s24bt_core_exit(void)/' \
	-e '/^subsys_initcall(bt_init);/d' -e '/^module_exit(bt_exit);/d' $D/bt/af_bluetooth.c
sed -i -e 's/^static int __init rfcomm_init(void)/int __init s24bt_rfcomm_init(void)/' \
	-e 's/^static void __exit rfcomm_exit(void)/void __exit s24bt_rfcomm_exit(void)/' \
	-e '/^module_init(rfcomm_init);/d' -e '/^module_exit(rfcomm_exit);/d' $D/rfcomm/core.c
sed -i -e 's/^static int __init hci_uart_init(void)/int __init s24bt_uart_init(void)/' \
	-e 's/^static void __exit hci_uart_exit(void)/void __exit s24bt_uart_exit(void)/' \
	-e '/^module_init(hci_uart_init);/d' -e '/^module_exit(hci_uart_exit);/d' $D/uart/hci_ldisc.c
grep -c 's24bt_core_init\|s24bt_core_exit' $D/bt/af_bluetooth.c
grep -c 's24bt_rfcomm_init\|s24bt_rfcomm_exit' $D/rfcomm/core.c
grep -c 's24bt_uart_init\|s24bt_uart_exit' $D/uart/hci_ldisc.c
grep -n 'initcall\|module_init\|module_exit' $D/bt/*.c $D/rfcomm/*.c $D/uart/*.c || true
