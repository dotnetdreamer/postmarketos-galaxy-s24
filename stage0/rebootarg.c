/* rebootarg: reboot with a reason string, e.g. "rebootarg download" puts a Samsung phone in
 * Download mode (the bootloader reads the reason). Static, for the S24 bring-up ramdisk. */
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/reboot.h>
int main(int argc, char **argv) {
	const char *arg = argc > 1 ? argv[1] : "download";
	sync();
	long r = syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART2, arg);
	perror("reboot");
	return (int)r;
}
