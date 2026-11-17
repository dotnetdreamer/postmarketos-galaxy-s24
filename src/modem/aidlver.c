/* aidlver (Claude, 2026-11-15): print the version and hash of AIDL services on /dev/binder,
 * read-only (getInterfaceVersion / getInterfaceHash; nothing is registered).
 *   aidlver [-l FILTER] | SERVICE IFACE... (SERVICE = "iface/instance") */
#include <gbinder.h>
#include <stdio.h>
#include <string.h>
static void query(GBinderServiceManager *sm, const char *fq)
{
	char iface[256];
	const char *slash = strrchr(fq, '/');
	int st = 0;
	GBinderRemoteObject *o = gbinder_servicemanager_get_service_sync(sm, fq, &st);
	GBinderClient *c;
	GBinderRemoteReply *rep;
	GBinderReader r;
	gint32 ex = -1, ver = -1;
	char *hash = NULL;
	if (!o) { printf("%s: not found (%d)\n", fq, st); return; }
	g_strlcpy(iface, fq, slash ? (gsize)(slash - fq) + 1 : sizeof iface);
	c = gbinder_client_new(o, iface);
	rep = gbinder_client_transact_sync_reply(c, 0x00ffffff, NULL, &st);
	if (rep) { gbinder_remote_reply_init_reader(rep, &r); gbinder_reader_read_int32(&r, &ex); gbinder_reader_read_int32(&r, &ver); gbinder_remote_reply_unref(rep); }
	rep = gbinder_client_transact_sync_reply(c, 0x00fffffe, NULL, &st);
	if (rep) { gbinder_remote_reply_init_reader(rep, &r); gbinder_reader_read_int32(&r, &ex); hash = gbinder_reader_read_string16(&r); gbinder_remote_reply_unref(rep); }
	printf("%s: version %d hash %s\n", fq, ver, hash ? hash : "?");
	g_free(hash);
	gbinder_client_unref(c);
}
int main(int argc, char **argv)
{
	GBinderServiceManager *sm = gbinder_servicemanager_new("/dev/binder");
	int i;
	if (!sm) { printf("no servicemanager\n"); return 1; }
	if (argc > 1 && !strcmp(argv[1], "-l")) {
		char **l = gbinder_servicemanager_list_sync(sm);
		for (i = 0; l && l[i]; i++)
			if (argc < 3 || strstr(l[i], argv[2]))
				printf("%s\n", l[i]);
		return 0;
	}
	for (i = 1; i < argc; i++)
		query(sm, argv[i]);
	return 0;
}
