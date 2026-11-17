/* Galaxy S24 modem container: LD_PRELOAD shim for servicemanager and vndservicemanager (Claude,
 * 2026-11-14). Both abort unless selinux_status_open() succeeds ("Check failed:
 * selinux_status_open(true) >= 0"), and it can only succeed with selinuxfs mounted, which would
 * make libselinux think SELinux is on: with no policy loaded the kernel then reports
 * deny_unknown=1 and every service_manager check is refused. Here SELinux stays off (no selinuxfs
 * in the container, so selinux_check_access() allows everything) and only the status check passes.
 * No libc: built with -nostdlib so bionic's linker can load it next to bionic. */
int selinux_status_open(int fallback)
{
	(void)fallback;
	return 0;
}

void selinux_status_close(void)
{
}

int selinux_status_updated(void)
{
	return 0;
}

int selinux_status_getenforce(void)
{
	return 0;
}

int selinux_status_policyload(void)
{
	return 0;
}

int selinux_status_deny_unknown(void)
{
	return 0;
}
