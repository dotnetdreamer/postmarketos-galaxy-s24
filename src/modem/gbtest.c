#include <gbinder.h>
#include <stdio.h>
int main(int argc, char **argv)
{
	GBinderServiceManager *sm = gbinder_servicemanager_new(argc > 2 ? argv[2] : "/dev/binder");
	char **l;
	int i, st = -1;
	GBinderRemoteObject *o;
	if (!sm) { printf("no sm\n"); return 1; }
	printf("present: %d\n", gbinder_servicemanager_is_present(sm));
	l = gbinder_servicemanager_list_sync(sm);
	for (i = 0; l && l[i]; i++) printf("  %s\n", l[i]);
	printf("%d listed\n", i);
	o = gbinder_servicemanager_get_service_sync(sm, argc > 1 ? argv[1] : "manager", &st);
	printf("get %s -> %p status %d\n", argc > 1 ? argv[1] : "manager", (void *)o, st);
	return 0;
}
