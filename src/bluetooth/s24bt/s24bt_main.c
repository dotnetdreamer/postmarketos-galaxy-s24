// SPDX-License-Identifier: GPL-2.0
/* Galaxy S24 postmarketOS (Claude, 2026-11-15): init/exit for s24bt, the self-contained Bluetooth
 * module (see s24bt_compat.h). The parts' own module_init/exit were renamed to these. */
#include <linux/module.h>

int s24bt_core_init(void);
void s24bt_core_exit(void);
int s24bt_rfcomm_init(void);
void s24bt_rfcomm_exit(void);
int s24bt_uart_init(void);
void s24bt_uart_exit(void);

static int __init s24bt_init(void)
{
	int err = s24bt_core_init();

	if (err)
		return err;
	err = s24bt_uart_init();
	if (err)
		goto core;
	err = s24bt_rfcomm_init();
	if (err)
		goto uart;
	return 0;
uart:
	s24bt_uart_exit();
core:
	s24bt_core_exit();
	return err;
}

static void __exit s24bt_exit(void)
{
	s24bt_rfcomm_exit();
	s24bt_uart_exit();
	s24bt_core_exit();
}

module_init(s24bt_init);
module_exit(s24bt_exit);
MODULE_DESCRIPTION("Bluetooth core + RFCOMM + HCI UART (H4) in one module, exporting nothing");
MODULE_LICENSE("GPL");
