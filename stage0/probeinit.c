/* probeinit: minimal /init for the Galaxy S24 bring-up (Claude, 2026-11-14).
 * Proves the kernel runs /init from the init_boot ramdisk: mounts /proc and /sys, logs to /dev/kmsg,
 * loads the first stock modules from the vendor ramdisk (up to exynos-reboot, which handles the
 * "download" reboot reason), then reboots into Download mode. A static binary on purpose: no shell
 * script, no interpreter, no busybox. If the reason is ignored it reboots plainly (a boot loop that
 * never reaches Upload mode, which also proves it ran). */
#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <linux/reboot.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>

static void klog(const char *s)
{
	int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (fd >= 0) {
		(void)!write(fd, s, strlen(s));
		close(fd);
	}
}

static int load(const char *dir, const char *name)
{
	char path[512];
	size_t n = strlen(name);
	if (n > 3 && !strcmp(name + n - 3, ".ko"))
		snprintf(path, sizeof path, "%s/%s", dir, name);
	else
		snprintf(path, sizeof path, "%s/%s.ko", dir, name);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1000;
	int r = (int)syscall(SYS_finit_module, fd, "", 0);
	close(fd);
	return r;
}

static FILE *open_modules_load(char *dir, size_t dirlen)
{
	snprintf(dir, dirlen, "/lib/modules");
	FILE *f = fopen("/lib/modules/modules.load", "r");
	if (f)
		return f;
	DIR *d = opendir("/lib/modules");
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char p[512];
		snprintf(p, sizeof p, "/lib/modules/%s/modules.load", e->d_name);
		if ((f = fopen(p, "r"))) {
			snprintf(dir, dirlen, "/lib/modules/%s", e->d_name);
			break;
		}
	}
	if (d)
		closedir(d);
	return f;
}

int main(void)
{
	char msg[600], dir[300];
	mkdir("/proc", 0755);
	mount("proc", "/proc", "proc", 0, NULL);
	mkdir("/sys", 0755);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	mkdir("/dev", 0755);
	mknod("/dev/kmsg", S_IFCHR | 0600, makedev(1, 11));
	klog("s24-probeinit: started as PID 1\n");

	FILE *f = open_modules_load(dir, sizeof dir);
	if (f) {
		char line[256];
		int count = 0;
		int saw_reboot = 0, saw_wdt = 0;
		while (count < 12 && fgets(line, sizeof line, f)) {
			line[strcspn(line, "\r\n")] = 0;
			if (!*line)
				continue;
			int r = load(dir, line);
			snprintf(msg, sizeof msg, "s24-probeinit: %s/%s -> %d\n", dir, line, r);
			klog(msg);
			count++;
			/* v2: stop only once both the reboot handler and the hardware watchdog driver are in,
			 * so the bootloader-started watchdog is taken over (kernel feeds it) before rebooting */
			if (strstr(line, "exynos-reboot") || strstr(line, "exynos_reboot"))
				saw_reboot = 1;
			if (strstr(line, "s3c2410_wdt") || strstr(line, "s3c2410-wdt"))
				saw_wdt = 1;
			if (saw_reboot && saw_wdt)
				break;
		}
		fclose(f);
	} else {
		klog("s24-probeinit: no modules.load\n");
	}
	sync();
	klog("s24-probeinit: reboot download\n");
	syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART2, "download");
	sleep(2);
	syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART, NULL);
	for (;;)
		pause();
}
