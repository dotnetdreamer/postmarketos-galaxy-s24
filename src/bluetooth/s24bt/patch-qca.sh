#!/bin/sh
# s24bt: add Qualcomm's HCI UART protocol (in-band sleep) to the module, with its firmware setup
# skipped for the line-discipline case (s24-bt loads the WCN7851 firmware itself)
set -e
K=/work/ksrc; D=/work/s24bt
cp $K/drivers/bluetooth/hci_qca.c $K/drivers/bluetooth/btqca.c $D/uart/
python3 - << 'PY'
p = '/work/s24bt/uart/hci_qca.c'
s = open(p).read()
old = """	ret = qca_check_speeds(hu);
	if (ret)
		return ret;

	clear_bit(QCA_ROM_FW, &qca->flags);"""
new = """	if (!hu->serdev) {
		/* s24: attached by s24-bt after it loaded the firmware and NVM itself (this kernel's
		 * btqca predates the WCN7850): nothing to download, just run in-band sleep */
		clear_bit(QCA_ROM_FW, &qca->flags);
		set_bit(HCI_QUIRK_SIMULTANEOUS_DISCOVERY, &hdev->quirks);
		clear_bit(QCA_IBS_DISABLED, &qca->flags);
		/* hci_uart_setup() has just put the host side at the protocol's init speed
		 * (115200); the chip already runs at the operational speed s24-bt set */
		hci_uart_set_baudrate(hu, qca_get_speed(hu, QCA_OPER_SPEED));
		bt_dev_info(hdev, "s24: firmware loaded by userspace, %u baud, in-band sleep on",
			    qca_get_speed(hu, QCA_OPER_SPEED));
		return 0;
	}

	ret = qca_check_speeds(hu);
	if (ret)
		return ret;

	clear_bit(QCA_ROM_FW, &qca->flags);"""
assert s.count(old) == 1
s = s.replace(old, new)
old2 = """int __init qca_init(void)
{
	serdev_device_driver_register(&qca_serdev_driver);

	return hci_uart_register_proto(&qca_proto);
}

int __exit qca_deinit(void)
{
	serdev_device_driver_unregister(&qca_serdev_driver);

	return hci_uart_unregister_proto(&qca_proto);
}"""
new2 = """int __init qca_init(void)
{
	/* s24: no serdev driver (no such device tree node; btpower owns the chip) */
	return hci_uart_register_proto(&qca_proto);
}

int __exit qca_deinit(void)
{
	return hci_uart_unregister_proto(&qca_proto);
}"""
assert s.count(old2) == 1
s = s.replace(old2, new2)
open(p, 'w').write(s)
PY
grep -n "s24:" $D/uart/hci_qca.c
