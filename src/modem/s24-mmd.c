/*
 * s24-mmd (Claude, 2026-11-15): the Galaxy S24's Samsung modem behind ModemManager's D-Bus API, for
 * postmarketOS. rild runs in the Android container (android-ctr0.sh) and offers Android's AIDL radio
 * HAL v2 over /dev/binder (wire format notes: s24-radio.c). ModemManager itself can't drive this
 * modem, so this daemon owns org.freedesktop.ModemManager1 in its place and exports one modem with
 * the Modem, Modem3gpp, Modem.Simple, Modem.Messaging and Modem.Voice interfaces, its SIM, one
 * bearer, text messages and calls, built from libmm-glib's generated skeletons (the same code
 * ModemManager is made of). NetworkManager then handles mobile data as for any modem (enable,
 * connect, IP setup on rmnet*), Plasma Mobile / Phosh show the signal, the operator and the mobile
 * data switch, and the SMS and dialer apps work as with any ModemManager modem.
 *
 * The modem is brought up the way Android's phone service does it: SIM status, IMSI, publish the SIM
 * operator (Samsung's RIL drops the initial attach APN without gsm.sim.operator.numeric, and without
 * that APN the modem never attaches to LTE), attach APN, data allowed, radio on. APN "" = from the
 * phone's own APN list (apns-conf.xml).
 *
 * DNS stays out of NetworkManager on purpose (musl asks all nameservers at once: mobile DNS64 answers
 * next to Wi-Fi's would mix): S24_MMD_HOOK (default /usr/local/sbin/s24-mobile-data-hook) is run as
 * "HOOK up IFACE DNS..." / "HOOK down IFACE" for DNS while there is no Wi-Fi, IPv4 through clatd and
 * the routing rule for NetworkManager's mobile table.
 *
 * Only one program may talk to rild at a time (the last one to register gets the answers): don't run
 * s24-radio while this runs.
 */
#include <libmm-glib.h>
#include <gbinder.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MM_PATH "/org/freedesktop/ModemManager1"
#define MODEM_PATH MM_PATH "/Modem/0"
#define SIM_PATH MM_PATH "/SIM/0"
#define BEARER_PATH MM_PATH "/Bearer/0"
#define APN_LIST "/android/system/etc/apns-conf.xml"
#define DEFAULT_HOOK "/usr/local/sbin/s24-mobile-data-hook"

/* ---------- RIL ------------------------------------------------------------------------------ */

enum { SIM, MODEM, NET, DATA, MSG, VOICE, NSVC };

static const struct {
	const char *name, *iface, *hash;
	guint32 set_resp, resp_ack;  /* setResponseFunctions, responseAcknowledgement */
	guint32 ack_req;             /* the response code of acknowledgeRequest */
} SVC[NSVC] = {
	{ "sim", "android.hardware.radio.sim.IRadioSim",
	  "4f348cc7aca716cc41c09ea95895c4b261231035", 28, 20, 1 },
	{ "modem", "android.hardware.radio.modem.IRadioModem",
	  "09927560afccc75a063944fbbab3af48099261ca", 17, 13, 1 },
	{ "network", "android.hardware.radio.network.IRadioNetwork",
	  "1b6608f238bd0b1c642df315621a7b605eafc883", 28, 16, 1 },
	{ "data", "android.hardware.radio.data.IRadioData",
	  "cb458326b02e0e87143f24118543e8cc7d6a9e8e", 12, 7, 1 },
	{ "messaging", "android.hardware.radio.messaging.IRadioMessaging",
	  "50aefda34c9dd40090c8d5925e71d5b84530c3d0", 20, 10, 4 },
	{ "voice", "android.hardware.radio.voice.IRadioVoice",
	  "8c5e0d53dc67b5ed221b2da0570a17684d973a20", 34, 23, 2 },
};

/* calls (radio.voice v2) */
#define VOICE_acceptCall 1
#define VOICE_conference 3
#define VOICE_dial 4
#define VOICE_emergencyDial 5
#define VOICE_explicitCallTransfer 7
#define VOICE_getCurrentCalls 12
#define VOICE_getLastCallFailCause 13
#define VOICE_hangup 18
#define VOICE_hangupForegroundResumeBackground 19
#define VOICE_hangupWaitingOrBackground 20
#define VOICE_sendDtmf 26
#define VOICE_separateConnection 28
#define VOICE_switchWaitingOrHoldingAndActive 39
#define VOICER_acceptCall 1
#define VOICER_conference 4
#define VOICER_dial 5
#define VOICER_emergencyDial 6
#define VOICER_explicitCallTransfer 8
#define VOICER_getCurrentCalls 13
#define VOICER_getLastCallFailCause 14
#define VOICER_hangupConnection 19
#define VOICER_hangupForegroundResumeBackground 20
#define VOICER_hangupWaitingOrBackground 21
#define VOICER_sendDtmf 26
#define VOICER_separateConnection 28
#define VOICER_switchWaitingOrHoldingAndActive 38
#define VOICEI_callRing 1
#define VOICEI_callStateChanged 2
#define VOICEI_currentEmergencyNumberList 6
#define VOICEI_indicateRingbackTone 9
#define VOICEI_srvccStateNotify 13

/* SMS (radio.messaging v2) */
#define MSG_acknowledgeLastIncomingGsmSms 3
#define MSG_reportSmsMemoryStatus 9
#define MSG_sendSms 14
#define MSG_sendSmsExpectMore 15
#define MSGR_acknowledgeLastIncomingGsmSms 3
#define MSGR_sendSmsExpectMore 14
#define MSGR_sendSms 15
#define MSGI_newSms 4
#define MSGI_newSmsStatusReport 6

/* requests (declaration index + 1 in the frozen aidl_api/<package>/2 files) */
#define SIM_getIccCardStatus 9
#define SIM_getImsiForApp 10
#define MODEM_getBasebandVersion 2
#define MODEM_setRadioPower 16
#define MODEM_getImei 18
#define NET_getAllowedNetworkTypesBitmap 1
#define NET_getDataRegistrationState 7
#define NET_getOperator 10
#define NET_getSignalStrength 11
#define NET_getVoiceRegistrationState 14
#define NET_setAllowedNetworkTypesBitmap 17
#define NET_setNetworkSelectionModeAutomatic 25
#define DATA_deactivateDataCall 3
#define DATA_getDataCallList 4
#define DATA_setDataAllowed 8
#define DATA_setInitialAttachApn 11
#define DATA_setupDataCall 13
/* responses */
#define SIMR_getIccCardStatus 10
#define SIMR_getImsiForApp 11
#define MODEMR_getBasebandVersion 3
#define MODEMR_setRadioPower 16
#define MODEMR_getImei 17
#define NETR_getAllowedNetworkTypesBitmap 2
#define NETR_getDataRegistrationState 8
#define NETR_getOperator 11
#define NETR_getSignalStrength 12
#define NETR_getVoiceRegistrationState 15
#define NETR_setAllowedNetworkTypesBitmap 17
#define NETR_setNetworkSelectionModeAutomatic 27
#define DATAR_deactivateDataCall 4
#define DATAR_getDataCallList 5
#define DATAR_setDataAllowed 8
#define DATAR_setInitialAttachApn 11
#define DATAR_setupDataCall 12
/* indications */
#define MODEMI_radioStateChanged 4
#define MODEMI_rilConnected 5
#define SIMI_simStatusChanged 6
#define NETI_currentSignalStrength 6
#define NETI_networkStateChanged 9
#define DATAI_dataCallListChanged 1

#define AIDL_GET_VERSION 0x00ffffff
#define AIDL_GET_HASH 0x00fffffe

/* RadioAccessFamily groups */
#define RAF_2G (65536 | 2 | 4)
#define RAF_3G (8 | 512 | 1024 | 2048 | 32768)
#define RAF_4G (16384 | 524288)
#define RAF_5G 1048576
#define UNSET 0x7fffffff

typedef struct { int svc; gboolean indication; } CbKind;

static GBinderServiceManager *sm;
static GBinderClient *client[NSVC];
static GBinderRemoteObject *remote[NSVC];
static GBinderLocalObject *cb_obj[NSVC][2];
static CbKind cb_kind[NSVC][2];
static gulong death_id[NSVC];
static int next_serial = 1;

static struct {
	gboolean up;                    /* connected to rild */
	int radio;                      /* RadioState: 0 off, 1 unavailable, 10 on; -1 unknown */
	int card, app_state, pin1;      /* -1 unknown */
	char *aid, *iccid, *imsi, *imei, *baseband;
	char hplmn[8];
	int data_reg, data_rat, voice_reg, voice_rat;
	char op_long[64], op_short[64], op_numeric[16];
	int lte_rsrp, nr_rsrp, gsm_asu;  /* dBm magnitudes / ASU, UNSET when unknown */
	int allowed;                    /* RadioAccessFamily bitmap, 0 unknown */
	char *ia_apn;                   /* the attach APN the modem was given */
	/* the data call */
	int cid;                        /* -1: none */
	char ifname[32];
	char **addrs, **dnses, **gws;
	int mtu;
} st = { .radio = -1, .card = -1, .app_state = -1, .pin1 = -1, .data_reg = -1, .data_rat = 0,
         .voice_reg = -1, .voice_rat = 0, .lte_rsrp = UNSET, .nr_rsrp = UNSET, .gsm_asu = UNSET,
         .cid = -1 };

/* ---------- ModemManager side ---------------------------------------------------------------- */

static GMainLoop *loop;
static GDBusConnection *bus;
static GDBusObjectManagerServer *om;
static MmGdbusOrgFreedesktopModemManager1 *mgr;
static MmGdbusObjectSkeleton *mobj;
static MmGdbusModem *modem;
static MmGdbusModem3gpp *m3gpp;
static MmGdbusModemSimple *simple;
static MmGdbusSim *simobj;
static MmGdbusBearer *bearer;
static MmGdbusModemVoice *voice;
static gboolean modem_exported, sim_exported, bearer_exported;
static MMModemState mm_state = MM_MODEM_STATE_UNKNOWN;

/* what is going on */
static enum { EN_NONE, EN_ATTACH_APN, EN_ALLOW, EN_POWER } en_step;
static gboolean en_target;                      /* enabling (TRUE) or disabling */
static GDBusMethodInvocation *inv_enable;
static enum { CO_NONE, CO_WAIT_ENABLE, CO_WAIT_REG, CO_ATTACH_APN, CO_SETUP } co_step;
static GDBusMethodInvocation *inv_connect;
static gboolean inv_connect_simple;             /* Simple.Connect (else Bearer.Connect) */
static char *co_apn;
static guint co_timer;
static GDBusMethodInvocation *inv_disconnect;
static gboolean inv_disconnect_simple;
static GDBusMethodInvocation *inv_modes;
static int pending_raf;


static void update_state(void);
static void update_modem_props(void);
static void connect_advance(void);
static void enable_advance(void);
static void export_modem(void);
static gboolean on_unsupported(GDBusInterfaceSkeleton *o, GDBusMethodInvocation *inv, gpointer u);

static void say(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void say(const char *fmt, ...)
{
	va_list ap;
	GDateTime *now = g_date_time_new_now_local();
	gchar *t = g_date_time_format(now, "%H:%M:%S");
	printf("%s ", t);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
	g_free(t);
	g_date_time_unref(now);
}

/* ---------- small RIL helpers (as s24-radio) ------------------------------------------------- */

static const char *radio_err(int e)
{
	switch (e) {
	case 0: return "NONE";
	case 1: return "RADIO_NOT_AVAILABLE";
	case 2: return "GENERIC_FAILURE";
	case 6: return "REQUEST_NOT_SUPPORTED";
	case 9: return "OP_NOT_ALLOWED_BEFORE_REG_TO_NW";
	case 11: return "SIM_ABSENT";
	case 38: return "INTERNAL_ERR";
	case 41: return "INVALID_STATE";
	case 44: return "INVALID_ARGUMENTS";
	case 46: return "INVALID_MODEM_STATE";
	}
	return "error";
}

static gboolean registered(int r) { return r == 1 || r == 5; }

static int access_network(int rat)  /* RadioTechnology -> AccessNetwork */
{
	switch (rat) {
	case 1: case 2: case 16: return 1;              /* GERAN */
	case 3: case 9: case 10: case 11: case 15: case 17: return 2;  /* UTRAN */
	case 14: case 19: return 3;                     /* EUTRAN */
	case 20: return 6;                              /* NGRAN */
	}
	return 3;
}

static GBinderLocalRequest *req_new(int s, GBinderWriter *w)
{
	GBinderLocalRequest *r = gbinder_client_new_request(client[s]);
	gbinder_local_request_init_writer(r, w);
	return r;
}

static int send_req(int s, guint32 code, GBinderLocalRequest *r)
{
	int status = gbinder_client_transact_sync_oneway(client[s], code, r);
	gbinder_local_request_unref(r);
	if (status != 0)
		say("ril: %s call %u failed: %d", SVC[s].name, code, status);
	return status;
}

static void call_serial(int s, guint32 code)
{
	GBinderWriter w;
	GBinderLocalRequest *r = req_new(s, &w);
	gbinder_writer_append_int32(&w, next_serial++);
	send_req(s, code, r);
}

static void ack(int s)
{
	GBinderWriter w;
	if (SVC[s].resp_ack)
		send_req(s, SVC[s].resp_ack, req_new(s, &w));
}

static gboolean read_info(GBinderReader *r, int *type, int *error)
{
	GBinderReader p;
	gboolean nn;
	gint32 t = 0, s = 0, e = 0;
	if (!gbinder_reader_start_parcelable(r, &p, &nn) || !nn)
		return FALSE;
	gbinder_reader_read_int32(&p, &t);
	gbinder_reader_read_int32(&p, &s);
	gbinder_reader_read_int32(&p, &e);
	gbinder_reader_finish_parcelable(&p);
	*type = t;
	*error = e;
	return TRUE;
}

static void skip_union(GBinderReader *r)  /* marker, tag, value (a parcelable unless tag 0) */
{
	gint32 marker = 0, tag = 0, v;
	gbinder_reader_read_int32(r, &marker);
	if (!marker)
		return;
	gbinder_reader_read_int32(r, &tag);
	if (tag == 0)
		gbinder_reader_read_int32(r, &v);
	else
		gbinder_reader_skip_parcelable(r);
}

static char **read_str_array(GBinderReader *r)
{
	gint32 n = 0, i;
	char **v;
	gbinder_reader_read_int32(r, &n);
	if (n < 0 || n > 64)
		n = 0;
	v = g_new0(char *, n + 1);
	for (i = 0; i < n; i++)
		v[i] = gbinder_reader_read_string16(r);
	return v;
}

static void set_radio_power(gboolean on)
{
	GBinderWriter w;
	GBinderLocalRequest *r = req_new(MODEM, &w);
	gbinder_writer_append_int32(&w, next_serial++);
	gbinder_writer_append_bool(&w, on);
	gbinder_writer_append_bool(&w, FALSE);  /* forEmergencyCall */
	gbinder_writer_append_bool(&w, FALSE);  /* preferredForEmergencyCall */
	send_req(MODEM, MODEM_setRadioPower, r);
}

static void get_imsi(void)
{
	GBinderWriter w;
	GBinderLocalRequest *r = req_new(SIM, &w);
	gbinder_writer_append_int32(&w, next_serial++);
	gbinder_writer_append_string16(&w, st.aid ? st.aid : "");
	send_req(SIM, SIM_getImsiForApp, r);
}

static void write_profile(GBinderWriter *w, const char *apn)  /* DataProfileInfo */
{
	GBinderWriter p, t;
	gbinder_writer_start_parcelable(w, &p);
	gbinder_writer_append_int32(&p, 0);             /* profileId: ID_DEFAULT */
	gbinder_writer_append_string16(&p, apn);
	gbinder_writer_append_int32(&p, 2);             /* protocol: IPV4V6 */
	gbinder_writer_append_int32(&p, 2);             /* roamingProtocol: IPV4V6 */
	gbinder_writer_append_int32(&p, 0);             /* authType: NO_PAP_NO_CHAP */
	gbinder_writer_append_string16(&p, "");         /* user */
	gbinder_writer_append_string16(&p, "");         /* password */
	gbinder_writer_append_int32(&p, 1);             /* type: TYPE_3GPP */
	gbinder_writer_append_int32(&p, 0);             /* maxConnsTime */
	gbinder_writer_append_int32(&p, 0);             /* maxConns */
	gbinder_writer_append_int32(&p, 0);             /* waitTime */
	gbinder_writer_append_bool(&p, TRUE);           /* enabled */
	gbinder_writer_append_int32(&p, 1 | 4 | 256);   /* ApnTypes: DEFAULT | SUPL | IA */
	gbinder_writer_append_int32(&p, 0);             /* bearerBitmap: any */
	gbinder_writer_append_int32(&p, 0);             /* mtuV4 */
	gbinder_writer_append_int32(&p, 0);             /* mtuV6 */
	gbinder_writer_append_bool(&p, TRUE);           /* preferred */
	gbinder_writer_append_bool(&p, TRUE);           /* persistent */
	gbinder_writer_append_bool(&p, FALSE);          /* alwaysOn */
	gbinder_writer_start_parcelable(&p, &t);        /* trafficDescriptor */
	gbinder_writer_append_string16(&t, NULL);       /*   dnn: null */
	gbinder_writer_append_int32(&t, 0);             /*   osAppId: null */
	gbinder_writer_finish_parcelable(&t);
	gbinder_writer_finish_parcelable(&p);
}

static void set_initial_attach_apn(const char *apn)
{
	GBinderWriter w;
	GBinderLocalRequest *r = req_new(DATA, &w);
	gbinder_writer_append_int32(&w, next_serial++);
	write_profile(&w, apn);
	g_free(st.ia_apn);
	st.ia_apn = g_strdup(apn);
	send_req(DATA, DATA_setInitialAttachApn, r);
}

static void set_data_allowed(gboolean allow)
{
	GBinderWriter w;
	GBinderLocalRequest *r = req_new(DATA, &w);
	gbinder_writer_append_int32(&w, next_serial++);
	gbinder_writer_append_bool(&w, allow);
	send_req(DATA, DATA_setDataAllowed, r);
}

static void setup_data_call(const char *apn)
{
	GBinderWriter w;
	GBinderLocalRequest *r = req_new(DATA, &w);
	gbinder_writer_append_int32(&w, next_serial++);
	gbinder_writer_append_int32(&w, access_network(st.data_rat));
	write_profile(&w, apn);
	gbinder_writer_append_bool(&w, TRUE);           /* roamingAllowed */
	gbinder_writer_append_int32(&w, 1);             /* reason: NORMAL */
	gbinder_writer_append_int32(&w, 0);             /* addresses: none */
	gbinder_writer_append_int32(&w, 0);             /* dnses: none */
	gbinder_writer_append_int32(&w, 0);             /* pduSessionId */
	gbinder_writer_append_int32(&w, 0);             /* sliceInfo: null */
	gbinder_writer_append_bool(&w, TRUE);           /* matchAllRuleAllowed */
	send_req(DATA, DATA_setupDataCall, r);
	say("data: setting up a call on APN \"%s\"", apn);
}

static void deactivate_data_call(int cid)
{
	GBinderWriter w;
	GBinderLocalRequest *r = req_new(DATA, &w);
	gbinder_writer_append_int32(&w, next_serial++);
	gbinder_writer_append_int32(&w, cid);
	gbinder_writer_append_int32(&w, 1);             /* reason: NORMAL */
	send_req(DATA, DATA_deactivateDataCall, r);
}

static void set_allowed_types(int raf)
{
	GBinderWriter w;
	GBinderLocalRequest *r = req_new(NET, &w);
	gbinder_writer_append_int32(&w, next_serial++);
	gbinder_writer_append_int32(&w, raf);
	send_req(NET, NET_setAllowedNetworkTypesBitmap, r);
}

static void query_registration(void)
{
	call_serial(NET, NET_getVoiceRegistrationState);
	call_serial(NET, NET_getDataRegistrationState);
	call_serial(NET, NET_getOperator);
	call_serial(NET, NET_getSignalStrength);
}

/* ---------- SIM operator and APN (as Android's phone service) -------------------------------- */

static int mnc_digits(const char *imsi)  /* Android's fallback when the SIM doesn't say (EF_AD) */
{
	static const int three[] = { 302, 310, 311, 312, 313, 314, 315, 316, 334, 338, 342, 344, 346,
	                             348, 354, 356, 358, 360, 365, 376, 405, 708, 722, 732 };
	char mcc[4] = { imsi[0], imsi[1], imsi[2], 0 };
	unsigned i;
	for (i = 0; i < G_N_ELEMENTS(three); i++)
		if (atoi(mcc) == three[i])
			return 3;
	return 2;
}

/* Samsung's RIL reads gsm.sim.operator.numeric when the attach APN arrives (one entry per slot) */
static void publish_sim_operator(void)
{
	const char *slot = getenv("S24_SLOT");
	int idx = slot && atoi(slot) == 2 ? 1 : 0;
	gchar *out = NULL, **v, *val;
	const gchar *e[2];
	if (!*st.hplmn)
		return;
	{
		gchar *args[] = { "chroot", "/android", "/system/bin/getprop", "gsm.sim.operator.numeric",
		                  NULL };
		if (!g_spawn_sync(NULL, args, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL,
		                  NULL, &out, NULL, NULL, NULL))
			out = NULL;
	}
	v = g_strsplit(out ? g_strstrip(out) : "", ",", 2);
	e[0] = v[0] ? v[0] : "";
	e[1] = v[0] && v[1] ? v[1] : "";
	e[idx] = st.hplmn;
	val = g_strjoin(",", e[0], e[1], NULL);
	{
		gchar *args[] = { "chroot", "/android", "/system/bin/setprop", "gsm.sim.operator.numeric",
		                  val, NULL };
		g_spawn_sync(NULL, args, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
		             G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, NULL, NULL);
	}
	say("sim: published operator %s (gsm.sim.operator.numeric %s)", st.hplmn, val);
	g_free(val);
	g_strfreev(v);
	g_free(out);
}

typedef struct { char mcc[4], mnc[4]; const char *imsi; char *apn, *carrier; int prio; } ApnPick;

static gboolean imsi_matches(const char *pat, const char *imsi)
{
	for (; *pat; pat++, imsi++)
		if (!*imsi || (*pat != 'x' && *pat != 'X' && *pat != *imsi))
			return FALSE;
	return TRUE;
}

static void apn_element(GMarkupParseContext *c, const char *el, const char **names,
                        const char **vals, gpointer data, GError **err)
{
	ApnPick *p = data;
	const char *mcc = NULL, *mnc = NULL, *apn = NULL, *type = NULL, *mvt = NULL, *mvd = NULL;
	const char *carrier = NULL;
	int i, prio;
	if (strcmp(el, "apn"))
		return;
	for (i = 0; names[i]; i++) {
		if (!strcmp(names[i], "mcc")) mcc = vals[i];
		else if (!strcmp(names[i], "mnc")) mnc = vals[i];
		else if (!strcmp(names[i], "apn")) apn = vals[i];
		else if (!strcmp(names[i], "type")) type = vals[i];
		else if (!strcmp(names[i], "mvno_type")) mvt = vals[i];
		else if (!strcmp(names[i], "mvno_match_data")) mvd = vals[i];
		else if (!strcmp(names[i], "carrier")) carrier = vals[i];
	}
	if (!mcc || !mnc || !apn || strcmp(mcc, p->mcc) || strcmp(mnc, p->mnc))
		return;
	if (type && !strstr(type, "default") && strcmp(type, "*"))
		return;
	if (mvt) {
		if (strcmp(mvt, "imsi") || !mvd || !p->imsi || !imsi_matches(mvd, p->imsi))
			return;
		prio = 2;
	} else {
		prio = 1;
	}
	if (prio > p->prio) {
		g_free(p->apn);
		g_free(p->carrier);
		p->apn = g_strdup(apn);
		p->carrier = g_strdup(carrier ? carrier : "?");
		p->prio = prio;
	}
}

/* the APN for this SIM from the phone's own list; "" if it has none (the network's default) */
static char *auto_apn(void)
{
	static const GMarkupParser parser = { apn_element, NULL, NULL, NULL, NULL };
	ApnPick p = { .imsi = st.imsi };
	gchar *xml = NULL;
	gsize len = 0;
	int n;
	if (!st.imsi || strlen(st.imsi) < 6)
		return g_strdup("");
	n = mnc_digits(st.imsi);
	memcpy(p.mcc, st.imsi, 3);
	memcpy(p.mnc, st.imsi + 3, n);
	if (g_file_get_contents(APN_LIST, &xml, &len, NULL)) {
		GMarkupParseContext *c = g_markup_parse_context_new(&parser, 0, &p, NULL);
		g_markup_parse_context_parse(c, xml, len, NULL);
		g_markup_parse_context_free(c);
		g_free(xml);
	}
	if (p.apn)
		say("data: APN \"%s\" from the phone's APN list (%s)", p.apn, p.carrier);
	g_free(p.carrier);
	return p.apn ? p.apn : g_strdup("");
}

/* ---------- hook (DNS without Wi-Fi, clatd, routing rule) ------------------------------------ */

static void run_hook(const char *what)
{
	const char *hook = getenv("S24_MMD_HOOK");
	GPtrArray *a = g_ptr_array_new();
	int i;
	if (!hook)
		hook = DEFAULT_HOOK;
	if (!*hook || !g_file_test(hook, G_FILE_TEST_IS_EXECUTABLE)) {
		g_ptr_array_free(a, TRUE);
		return;
	}
	g_ptr_array_add(a, (gpointer)hook);
	g_ptr_array_add(a, (gpointer)what);
	g_ptr_array_add(a, st.ifname[0] ? st.ifname : "-");
	for (i = 0; !strcmp(what, "up") && st.dnses && st.dnses[i]; i++)
		g_ptr_array_add(a, st.dnses[i]);
	g_ptr_array_add(a, NULL);
	if (!g_spawn_sync(NULL, (gchar **)a->pdata, NULL, G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL,
	                  NULL, NULL, NULL, NULL))
		say("hook: %s failed to run", hook);
	else
		say("hook: %s %s", what, st.ifname);
	g_ptr_array_free(a, TRUE);
}

/* ---------- ModemManager properties ---------------------------------------------------------- */

static guint access_tech(int rat)
{
	switch (rat) {
	case 1: return MM_MODEM_ACCESS_TECHNOLOGY_GPRS;
	case 2: return MM_MODEM_ACCESS_TECHNOLOGY_EDGE;
	case 3: return MM_MODEM_ACCESS_TECHNOLOGY_UMTS;
	case 9: return MM_MODEM_ACCESS_TECHNOLOGY_HSDPA;
	case 10: return MM_MODEM_ACCESS_TECHNOLOGY_HSUPA;
	case 11: return MM_MODEM_ACCESS_TECHNOLOGY_HSPA;
	case 15: return MM_MODEM_ACCESS_TECHNOLOGY_HSPA_PLUS;
	case 16: return MM_MODEM_ACCESS_TECHNOLOGY_GSM;
	case 14: case 19: return MM_MODEM_ACCESS_TECHNOLOGY_LTE;
	case 20: return MM_MODEM_ACCESS_TECHNOLOGY_5GNR;
	}
	return MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
}

static MMModem3gppRegistrationState reg_3gpp(int r)
{
	switch (r) {
	case 0: return MM_MODEM_3GPP_REGISTRATION_STATE_IDLE;
	case 1: return MM_MODEM_3GPP_REGISTRATION_STATE_HOME;
	case 2: return MM_MODEM_3GPP_REGISTRATION_STATE_SEARCHING;
	case 3: return MM_MODEM_3GPP_REGISTRATION_STATE_DENIED;
	case 5: return MM_MODEM_3GPP_REGISTRATION_STATE_ROAMING;
	case 10: case 12: case 13: case 14: case 20: return MM_MODEM_3GPP_REGISTRATION_STATE_IDLE;
	}
	return MM_MODEM_3GPP_REGISTRATION_STATE_UNKNOWN;
}

static guint raf_to_modes(int raf)
{
	guint m = 0;
	if (raf & RAF_2G) m |= MM_MODEM_MODE_2G;
	if (raf & RAF_3G) m |= MM_MODEM_MODE_3G;
	if (raf & RAF_4G) m |= MM_MODEM_MODE_4G;
	if (raf & RAF_5G) m |= MM_MODEM_MODE_5G;
	return m;
}

static int modes_to_raf(guint m)
{
	int raf = 0;
	if (m & MM_MODEM_MODE_2G) raf |= RAF_2G;
	if (m & MM_MODEM_MODE_3G) raf |= RAF_3G;
	if (m & MM_MODEM_MODE_4G) raf |= RAF_4G;
	if (m & MM_MODEM_MODE_5G) raf |= RAF_5G;
	return raf;
}

static int pct(int dbm_mag)  /* RSRP magnitude -> %, -120 dBm = 0 .. -80 dBm = 100 */
{
	int q = (120 - dbm_mag) * 100 / 40;
	return q < 0 ? 0 : q > 100 ? 100 : q;
}

static void update_signal(void)
{
	static int last = -2;
	int q = -1, reg = registered(st.data_reg) || registered(st.voice_reg);
	if (st.lte_rsrp != UNSET && st.lte_rsrp > 0)
		q = pct(st.lte_rsrp);
	if (st.nr_rsrp != UNSET && st.nr_rsrp > 0 && pct(st.nr_rsrp) > q)
		q = pct(st.nr_rsrp);
	if (q < 0 && st.gsm_asu != UNSET && st.gsm_asu >= 0 && st.gsm_asu <= 31)
		q = st.gsm_asu * 100 / 31;
	if (q < 0 || !reg)
		q = 0;
	if (q != last && modem) {
		last = q;
		mm_gdbus_modem_set_signal_quality(modem, g_variant_new("(ub)", (guint)q, TRUE));
	}
}

static void update_registration(void)
{
	int reg = registered(st.data_reg) ? st.data_reg : st.voice_reg;
	int rat = registered(st.data_reg) ? st.data_rat : st.voice_rat;
	guint at = registered(reg) ? access_tech(rat) : MM_MODEM_ACCESS_TECHNOLOGY_UNKNOWN;
	if (at == MM_MODEM_ACCESS_TECHNOLOGY_LTE && st.nr_rsrp != UNSET && st.nr_rsrp > 0)
		at |= MM_MODEM_ACCESS_TECHNOLOGY_5GNR;     /* 5G next to LTE (NSA) */
	if (!modem)
		return;
	mm_gdbus_modem_set_access_technologies(modem, at);
	mm_gdbus_modem3gpp_set_registration_state(m3gpp, reg_3gpp(reg < 0 ? 4 : reg));
	mm_gdbus_modem3gpp_set_operator_code(m3gpp, registered(reg) ? st.op_numeric : "");
	mm_gdbus_modem3gpp_set_operator_name(m3gpp, registered(reg) ?
	                                     (*st.op_long ? st.op_long : st.op_short) : "");
	mm_gdbus_modem3gpp_set_packet_service_state(m3gpp, registered(st.data_reg) ?
	                                            MM_MODEM_3GPP_PACKET_SERVICE_STATE_ATTACHED :
	                                            MM_MODEM_3GPP_PACKET_SERVICE_STATE_DETACHED);
	if (voice)
		mm_gdbus_modem_voice_set_emergency_only(voice, st.voice_reg >= 0 && !registered(st.voice_reg));
	update_signal();
}

static void set_state(MMModemState s, MMModemStateFailedReason why)
{
	MMModemState old = mm_state;
	if (s == mm_state || !modem)
		return;
	mm_state = s;
	mm_gdbus_modem_set_state(modem, s);
	mm_gdbus_modem_set_state_failed_reason(modem, s == MM_MODEM_STATE_FAILED ? why :
	                                       MM_MODEM_STATE_FAILED_REASON_NONE);
	mm_gdbus_modem_emit_state_changed(modem, old, s, MM_MODEM_STATE_CHANGE_REASON_UNKNOWN);
	say("mm: state %s -> %s", mm_modem_state_get_string(old), mm_modem_state_get_string(s));
}

static void update_state(void)
{
	MMModemState s;
	MMModemStateFailedReason why = MM_MODEM_STATE_FAILED_REASON_NONE;
	if (!modem)
		return;
	if (!st.up)
		s = MM_MODEM_STATE_UNKNOWN;
	else if (st.card == 0) {
		s = MM_MODEM_STATE_FAILED;
		why = MM_MODEM_STATE_FAILED_REASON_SIM_MISSING;
	} else if (st.card == 2) {
		s = MM_MODEM_STATE_FAILED;
		why = MM_MODEM_STATE_FAILED_REASON_SIM_ERROR;
	} else if (st.app_state == 2 || st.app_state == 3 || st.app_state == 4)
		s = MM_MODEM_STATE_LOCKED;
	else if (st.app_state != 5)
		s = MM_MODEM_STATE_INITIALIZING;
	else if (st.radio != 10)
		s = inv_enable && en_target ? MM_MODEM_STATE_ENABLING : MM_MODEM_STATE_DISABLED;
	else if (inv_enable && !en_target)
		s = MM_MODEM_STATE_DISABLING;
	else if (st.cid >= 0)
		s = inv_disconnect ? MM_MODEM_STATE_DISCONNECTING : MM_MODEM_STATE_CONNECTED;
	else if (co_step != CO_NONE)
		s = MM_MODEM_STATE_CONNECTING;
	else if (registered(st.data_reg) || registered(st.voice_reg))
		s = MM_MODEM_STATE_REGISTERED;
	else if (st.data_reg == 2 || st.voice_reg == 2)
		s = MM_MODEM_STATE_SEARCHING;
	else
		s = MM_MODEM_STATE_ENABLED;
	mm_gdbus_modem_set_unlock_required(modem, st.app_state == 2 ? MM_MODEM_LOCK_SIM_PIN :
	                                   st.app_state == 3 ? MM_MODEM_LOCK_SIM_PUK :
	                                   st.app_state == 5 ? MM_MODEM_LOCK_NONE :
	                                   MM_MODEM_LOCK_UNKNOWN);
	mm_gdbus_modem_set_power_state(modem, st.radio == 10 ? MM_MODEM_POWER_STATE_ON :
	                               MM_MODEM_POWER_STATE_LOW);
	set_state(s, why);
}

static void update_modem_props(void)
{
	const gchar *const drivers[] = { "samsung-ril", NULL };
	const gchar *const none[] = { NULL };
	GVariantBuilder b;
	guint all = MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G | MM_MODEM_MODE_5G;
	if (!modem)
		return;
	mm_gdbus_modem_set_manufacturer(modem, "Samsung");
	mm_gdbus_modem_set_model(modem, "Galaxy S24 (Exynos)");
	mm_gdbus_modem_set_revision(modem, st.baseband ? st.baseband : "");
	mm_gdbus_modem_set_hardware_revision(modem, "");
	mm_gdbus_modem_set_equipment_identifier(modem, st.imei ? st.imei : "");
	mm_gdbus_modem_set_device_identifier(modem, "s24-samsung-ril");
	mm_gdbus_modem_set_device(modem, "/sys/devices/virtual/net/rmnet0");
	mm_gdbus_modem_set_physdev(modem, "/sys/devices/virtual/net/rmnet0");
	mm_gdbus_modem_set_drivers(modem, drivers);
	mm_gdbus_modem_set_plugin(modem, "s24-mmd");
	mm_gdbus_modem_set_primary_port(modem, "s24-ril");
	g_variant_builder_init(&b, G_VARIANT_TYPE("a(su)"));
	g_variant_builder_add(&b, "(su)", "s24-ril", (guint)MM_MODEM_PORT_TYPE_UNKNOWN);
	g_variant_builder_add(&b, "(su)", "rmnet0", (guint)MM_MODEM_PORT_TYPE_NET);
	mm_gdbus_modem_set_ports(modem, g_variant_builder_end(&b));
	g_variant_builder_init(&b, G_VARIANT_TYPE("au"));
	g_variant_builder_add(&b, "u", (guint)(MM_MODEM_CAPABILITY_GSM_UMTS | MM_MODEM_CAPABILITY_LTE |
	                                       MM_MODEM_CAPABILITY_5GNR));
	mm_gdbus_modem_set_supported_capabilities(modem, g_variant_builder_end(&b));
	mm_gdbus_modem_set_current_capabilities(modem, MM_MODEM_CAPABILITY_GSM_UMTS |
	                                        MM_MODEM_CAPABILITY_LTE | MM_MODEM_CAPABILITY_5GNR);
	mm_gdbus_modem_set_max_bearers(modem, 1);
	mm_gdbus_modem_set_max_active_bearers(modem, 1);
	g_variant_builder_init(&b, G_VARIANT_TYPE("a(uu)"));
	g_variant_builder_add(&b, "(uu)", all, (guint)MM_MODEM_MODE_NONE);
	g_variant_builder_add(&b, "(uu)", (guint)(MM_MODEM_MODE_2G | MM_MODEM_MODE_3G | MM_MODEM_MODE_4G),
	                      (guint)MM_MODEM_MODE_NONE);
	g_variant_builder_add(&b, "(uu)", (guint)(MM_MODEM_MODE_4G | MM_MODEM_MODE_5G),
	                      (guint)MM_MODEM_MODE_NONE);
	g_variant_builder_add(&b, "(uu)", (guint)MM_MODEM_MODE_4G, (guint)MM_MODEM_MODE_NONE);
	g_variant_builder_add(&b, "(uu)", (guint)MM_MODEM_MODE_2G, (guint)MM_MODEM_MODE_NONE);
	mm_gdbus_modem_set_supported_modes(modem, g_variant_builder_end(&b));
	mm_gdbus_modem_set_current_modes(modem, g_variant_new("(uu)", st.allowed ? raf_to_modes(st.allowed) : all,
	                                                      (guint)MM_MODEM_MODE_NONE));
	g_variant_builder_init(&b, G_VARIANT_TYPE("au"));
	g_variant_builder_add(&b, "u", (guint)MM_MODEM_BAND_ANY);
	mm_gdbus_modem_set_supported_bands(modem, g_variant_builder_end(&b));
	g_variant_builder_init(&b, G_VARIANT_TYPE("au"));
	g_variant_builder_add(&b, "u", (guint)MM_MODEM_BAND_ANY);
	mm_gdbus_modem_set_current_bands(modem, g_variant_builder_end(&b));
	mm_gdbus_modem_set_supported_ip_families(modem, MM_BEARER_IP_FAMILY_IPV4 |
	                                         MM_BEARER_IP_FAMILY_IPV6 | MM_BEARER_IP_FAMILY_IPV4V6);
	mm_gdbus_modem_set_unlock_retries(modem, g_variant_new_array(G_VARIANT_TYPE("{uu}"), NULL, 0));
	mm_gdbus_modem_set_own_numbers(modem, none);
	mm_gdbus_modem_set_sim_slots(modem, none);
	mm_gdbus_modem_set_primary_sim_slot(modem, 0);
	mm_gdbus_modem_set_sim(modem, sim_exported ? SIM_PATH : "/");
	mm_gdbus_modem_set_carrier_configuration(modem, "");
	mm_gdbus_modem_set_carrier_configuration_revision(modem, "");
	mm_gdbus_modem3gpp_set_imei(m3gpp, st.imei ? st.imei : "");
	mm_gdbus_modem3gpp_set_enabled_facility_locks(m3gpp, MM_MODEM_3GPP_FACILITY_NONE);
	mm_gdbus_modem3gpp_set_eps_ue_mode_operation(m3gpp, MM_MODEM_3GPP_EPS_UE_MODE_OPERATION_PS_2);
	update_registration();
}

static void update_sim(void)
{
	const gchar *const emergency[] = { "112", "911", NULL };
	if (!simobj || !modem_exported)
		return;
	mm_gdbus_sim_set_active(simobj, TRUE);
	mm_gdbus_sim_set_sim_identifier(simobj, st.iccid ? st.iccid : "");
	mm_gdbus_sim_set_imsi(simobj, st.imsi ? st.imsi : "");
	mm_gdbus_sim_set_operator_identifier(simobj, st.hplmn);
	mm_gdbus_sim_set_operator_name(simobj, registered(st.data_reg) && !strcmp(st.op_numeric, st.hplmn) ?
	                               (*st.op_short ? st.op_short : st.op_long) : "");
	mm_gdbus_sim_set_emergency_numbers(simobj, emergency);
	mm_gdbus_sim_set_sim_type(simobj, MM_SIM_TYPE_PHYSICAL);
	mm_gdbus_sim_set_removability(simobj, MM_SIM_REMOVABILITY_REMOVABLE);
	if (!sim_exported && st.card == 1) {
		GError *err = NULL;
		if (g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(simobj), bus, SIM_PATH, &err)) {
			sim_exported = TRUE;
			mm_gdbus_modem_set_sim(modem, SIM_PATH);
		} else {
			say("mm: SIM export failed: %s", err->message);
			g_error_free(err);
		}
	}
}

/* the bearer's IP settings for NetworkManager (static: what the network gave the call) */
static GVariant *ip_config(gboolean v6)
{
	GVariantBuilder b;
	int i;
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	for (i = 0; st.cid >= 0 && st.addrs && st.addrs[i]; i++) {
		gchar **parts;
		gboolean is6 = strchr(st.addrs[i], ':') != NULL;
		if (is6 != v6)
			continue;
		parts = g_strsplit(st.addrs[i], "/", 2);
		g_variant_builder_add(&b, "{sv}", "method", g_variant_new_uint32(MM_BEARER_IP_METHOD_STATIC));
		g_variant_builder_add(&b, "{sv}", "address", g_variant_new_string(parts[0]));
		g_variant_builder_add(&b, "{sv}", "prefix",
		                      g_variant_new_uint32(parts[1] ? atoi(parts[1]) : v6 ? 64 : 32));
		for (gint j = 0; st.gws && st.gws[j]; j++)
			if ((strchr(st.gws[j], ':') != NULL) == v6) {
				g_variant_builder_add(&b, "{sv}", "gateway", g_variant_new_string(st.gws[j]));
				break;
			}
		if (st.mtu > 0)
			g_variant_builder_add(&b, "{sv}", "mtu", g_variant_new_uint32(st.mtu));
		/* no dns1/dns2: the hook gives the mobile DNS only while Wi-Fi gives none (musl) */
		g_strfreev(parts);
		return g_variant_builder_end(&b);
	}
	/* no address of this family: say so, as ModemManager does (an empty one is "invalid") */
	g_variant_builder_add(&b, "{sv}", "method", g_variant_new_uint32(MM_BEARER_IP_METHOD_UNKNOWN));
	return g_variant_builder_end(&b);
}

static void update_bearer(void)
{
	GVariantBuilder b;
	const gchar *const list[] = { BEARER_PATH, NULL };
	const gchar *const none[] = { NULL };
	if (!bearer)
		return;
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&b, "{sv}", "apn", g_variant_new_string(co_apn ? co_apn :
	                                                              st.ia_apn ? st.ia_apn : ""));
	g_variant_builder_add(&b, "{sv}", "ip-type", g_variant_new_uint32(MM_BEARER_IP_FAMILY_IPV4V6));
	g_variant_builder_add(&b, "{sv}", "allow-roaming", g_variant_new_boolean(TRUE));
	mm_gdbus_bearer_set_properties(bearer, g_variant_builder_end(&b));
	mm_gdbus_bearer_set_interface(bearer, st.cid >= 0 ? st.ifname : "");
	mm_gdbus_bearer_set_ip4_config(bearer, ip_config(FALSE));
	mm_gdbus_bearer_set_ip6_config(bearer, ip_config(TRUE));
	mm_gdbus_bearer_set_connected(bearer, st.cid >= 0);
	if (!bearer_exported && modem_exported) {
		GError *err = NULL;
		if (g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(bearer), bus, BEARER_PATH, &err))
			bearer_exported = TRUE;
		else {
			say("mm: bearer export failed: %s", err->message);
			g_error_free(err);
		}
	}
	mm_gdbus_modem_set_bearers(modem, bearer_exported ? list : none);
}

/* ---------- connect / disconnect / enable ---------------------------------------------------- */

static void fail_connect(const char *why)
{
	say("data: connect failed: %s", why);
	if (inv_connect)
		g_dbus_method_invocation_return_error(inv_connect, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
		                                      "%s", why);
	inv_connect = NULL;
	co_step = CO_NONE;
	if (co_timer) {
		g_source_remove(co_timer);
		co_timer = 0;
	}
	update_state();
}

static void finish_connect(void)
{
	if (inv_connect) {
		if (inv_connect_simple)
			mm_gdbus_modem_simple_complete_connect(simple, inv_connect, BEARER_PATH);
		else
			mm_gdbus_bearer_complete_connect(bearer, inv_connect);
	}
	inv_connect = NULL;
	co_step = CO_NONE;
	if (co_timer) {
		g_source_remove(co_timer);
		co_timer = 0;
	}
	update_state();
}

static gboolean connect_timeout(gpointer unused)
{
	co_timer = 0;
	fail_connect("timed out");
	return G_SOURCE_REMOVE;
}

/* SIM ready -> enabled -> registered -> (attach APN) -> data call */
static void connect_advance(void)
{
	if (co_step == CO_NONE)
		return;
	if (co_step == CO_WAIT_ENABLE) {
		if (st.radio != 10)
			return;
		co_step = CO_WAIT_REG;
	}
	if (co_step == CO_WAIT_REG) {
		if (!registered(st.data_reg))
			return;
		if (!st.ia_apn || strcmp(st.ia_apn, co_apn)) {
			co_step = CO_ATTACH_APN;
			set_initial_attach_apn(co_apn);
			return;
		}
		co_step = CO_SETUP;
		setup_data_call(co_apn);
	}
}

static void start_enable(gboolean on);

static gboolean start_connect(GDBusMethodInvocation *inv, gboolean is_simple, GVariant *props)
{
	const char *apn = NULL;
	if (inv_connect) {
		g_dbus_method_invocation_return_error(inv, MM_CORE_ERROR, MM_CORE_ERROR_IN_PROGRESS,
		                                      "a connection is already being set up");
		return TRUE;
	}
	if (props)
		g_variant_lookup(props, "apn", "&s", &apn);
	if (st.cid >= 0) {          /* already up: hand out the same bearer */
		if (is_simple)
			mm_gdbus_modem_simple_complete_connect(simple, inv, BEARER_PATH);
		else
			mm_gdbus_bearer_complete_connect(bearer, inv);
		return TRUE;
	}
	if (st.card != 1 || st.app_state != 5) {
		g_dbus_method_invocation_return_error(inv, MM_CORE_ERROR, MM_CORE_ERROR_WRONG_STATE,
		                                      "the SIM is not ready");
		return TRUE;
	}
	g_free(co_apn);
	co_apn = apn && *apn ? g_strdup(apn) : auto_apn();
	inv_connect = inv;
	inv_connect_simple = is_simple;
	co_timer = g_timeout_add_seconds(90, connect_timeout, NULL);
	say("data: connect asked (APN \"%s\")", co_apn);
	if (st.radio != 10) {
		co_step = CO_WAIT_ENABLE;
		if (!inv_enable)
			start_enable(TRUE);
	} else {
		co_step = CO_WAIT_REG;
	}
	update_state();
	connect_advance();
	return TRUE;
}

static void bearer_gone(const char *why)
{
	say("data: call gone (%s)", why);
	run_hook("down");
	st.cid = -1;
	g_strfreev(st.addrs); g_strfreev(st.dnses); g_strfreev(st.gws);
	st.addrs = st.dnses = st.gws = NULL;
	update_bearer();
	update_state();
}

static gboolean start_disconnect(GDBusMethodInvocation *inv, gboolean is_simple)
{
	if (inv_connect)
		fail_connect("cancelled by a disconnect");
	if (st.cid < 0) {
		if (is_simple)
			mm_gdbus_modem_simple_complete_disconnect(simple, inv);
		else
			mm_gdbus_bearer_complete_disconnect(bearer, inv);
		return TRUE;
	}
	if (inv_disconnect) {
		g_dbus_method_invocation_return_error(inv, MM_CORE_ERROR, MM_CORE_ERROR_IN_PROGRESS,
		                                      "already disconnecting");
		return TRUE;
	}
	inv_disconnect = inv;
	inv_disconnect_simple = is_simple;
	run_hook("down");
	deactivate_data_call(st.cid);
	update_state();
	return TRUE;
}

static void finish_disconnect(void)
{
	GDBusMethodInvocation *inv = inv_disconnect;
	inv_disconnect = NULL;
	if (st.cid >= 0)
		bearer_gone("disconnected");
	if (inv) {
		if (inv_disconnect_simple)
			mm_gdbus_modem_simple_complete_disconnect(simple, inv);
		else
			mm_gdbus_bearer_complete_disconnect(bearer, inv);
	}
}

/* enabling = Android's order: operator, attach APN, data allowed, radio on */
static void start_enable(gboolean on)
{
	en_target = on;
	if (on) {
		char *apn;
		publish_sim_operator();
		apn = co_apn ? g_strdup(co_apn) : auto_apn();
		en_step = EN_ATTACH_APN;
		set_initial_attach_apn(apn);
		g_free(apn);
	} else {
		if (st.cid >= 0) {
			run_hook("down");
			deactivate_data_call(st.cid);
		}
		en_step = EN_POWER;
		set_radio_power(FALSE);
	}
	update_state();
}

static void finish_enable(gboolean ok, const char *why)
{
	GDBusMethodInvocation *inv = inv_enable;
	inv_enable = NULL;
	en_step = EN_NONE;
	if (inv) {
		if (ok)
			mm_gdbus_modem_complete_enable(modem, inv);
		else
			g_dbus_method_invocation_return_error(inv, MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "%s", why);
	}
	if (ok && en_target)
		query_registration();
	update_state();
	connect_advance();
}

static void enable_advance(void)
{
	switch (en_step) {
	case EN_ATTACH_APN:
		en_step = EN_ALLOW;
		set_data_allowed(TRUE);
		break;
	case EN_ALLOW:
		en_step = EN_POWER;
		set_radio_power(TRUE);
		break;
	default:
		break;
	}
}

/* ---------- SMS ------------------------------------------------------------------------------
 * ModemManager's Messaging interface over radio.messaging: received SMS-DELIVER PDUs are acked at
 * once, decoded (GSM 7-bit with the extension table, 8-bit, UCS-2; parts joined by their UDH) and
 * published as SMS objects; Create + Sms.Send encode SMS-SUBMIT PDUs (7-bit when every character
 * fits, else UCS-2; split with an 8-bit-reference UDH) and send them part by part. Messages live in
 * memory until a client (Spacebar) deletes them. */

#define SMS_PATH MM_PATH "/SMS/"

static const gunichar gsm7[128] = {
	'@', 0xA3, '$', 0xA5, 0xE8, 0xE9, 0xF9, 0xEC, 0xF2, 0xC7, '\n', 0xD8, 0xF8, '\r', 0xC5, 0xE5,
	0x394, '_', 0x3A6, 0x393, 0x39B, 0x3A9, 0x3A0, 0x3A8, 0x3A3, 0x398, 0x39E, 0x1B, 0xC6, 0xE6, 0xDF, 0xC9,
	' ', '!', '"', '#', 0xA4, '%', '&', '\'', '(', ')', '*', '+', ',', '-', '.', '/',
	'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', ';', '<', '=', '>', '?',
	0xA1, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',
	'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 0xC4, 0xD6, 0xD1, 0xDC, 0xA7,
	0xBF, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o',
	'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', 0xE4, 0xF6, 0xF1, 0xFC, 0xE0,
};
static const struct { guint8 code; gunichar ch; } gsm7x[] = {   /* after ESC (0x1B) */
	{ 0x0A, 0x0C }, { 0x14, '^' }, { 0x28, '{' }, { 0x29, '}' }, { 0x2F, '\\' }, { 0x3C, '[' },
	{ 0x3D, '~' }, { 0x3E, ']' }, { 0x40, '|' }, { 0x65, 0x20AC },
};

static int gsm7_code(gunichar c, int *ext)   /* septet for c, -1 if it has none */
{
	unsigned i;
	*ext = 0;
	for (i = 0; i < 128; i++)
		if (gsm7[i] == c && i != 0x1B)
			return i;
	for (i = 0; i < G_N_ELEMENTS(gsm7x); i++)
		if (gsm7x[i].ch == c) {
			*ext = 1;
			return gsm7x[i].code;
		}
	return -1;
}

static int septet_at(const guint8 *b, gsize len, gsize i)
{
	gsize bit = i * 7, o = bit / 8;
	int s = bit % 8, v;
	if (o >= len)
		return -1;
	v = b[o] >> s;
	if (s > 1 && o + 1 < len)
		v |= b[o + 1] << (8 - s);
	return v & 0x7F;
}

static void put_septet(guint8 *b, gsize i, int v)
{
	gsize bit = i * 7, o = bit / 8;
	int s = bit % 8;
	b[o] |= (guint8)(v << s);
	if (s > 1)
		b[o + 1] |= (guint8)(v >> (8 - s));
}

/* septets [from, from+n) of a packed stream -> UTF-8 */
static char *gsm7_text(const guint8 *b, gsize len, gsize from, gsize n)
{
	GString *t = g_string_new(NULL);
	gsize i;
	for (i = from; i < from + n; i++) {
		int v = septet_at(b, len, i);
		if (v < 0)
			break;
		if (v == 0x1B && i + 1 < from + n) {
			int x = septet_at(b, len, ++i);
			unsigned k;
			gunichar ch = ' ';
			for (k = 0; k < G_N_ELEMENTS(gsm7x); k++)
				if (gsm7x[k].code == x)
					ch = gsm7x[k].ch;
			g_string_append_unichar(t, ch);
		} else {
			g_string_append_unichar(t, gsm7[v]);
		}
	}
	return g_string_free(t, FALSE);
}

static char *ucs2_text(const guint8 *b, gsize n)
{
	gunichar2 *u = g_new0(gunichar2, n / 2 + 1);
	char *t;
	gsize i;
	for (i = 0; i + 1 < n; i += 2)
		u[i / 2] = (gunichar2)(b[i] << 8 | b[i + 1]);
	t = g_utf16_to_utf8(u, n / 2, NULL, NULL, NULL);
	g_free(u);
	return t ? t : g_strdup("");
}

/* semi-octet digits (low nibble first) -> "+1555..." */
static char *bcd_number(guint8 toa, const guint8 *b, gsize ndigits)
{
	GString *n = g_string_new((toa & 0x70) == 0x10 ? "+" : "");
	gsize i;
	for (i = 0; i < ndigits; i++) {
		int d = i % 2 ? b[i / 2] >> 4 : b[i / 2] & 0x0F;
		if (d == 0x0F)
			break;
		g_string_append_c(n, "0123456789*#abc"[d]);
	}
	return g_string_free(n, FALSE);
}

static int bcd2(guint8 b) { return (b & 0x0F) * 10 + (b >> 4); }

typedef struct {
	char *number, *text, *smsc, *timestamp;
	int klass;
	guint ref, total, seq;   /* total 0: a single message */
} SmsPart;

static void sms_part_free(SmsPart *p)
{
	g_free(p->number); g_free(p->text); g_free(p->smsc); g_free(p->timestamp);
	g_free(p);
}

/* SMSC address + SMS-DELIVER TPDU (TS 23.040), as newSms hands them over */
static SmsPart *decode_deliver(const guint8 *b, gsize len)
{
	SmsPart *p = g_new0(SmsPart, 1);
	gsize o = 0, smsc_len, oa_digits, udl, ud_len;
	guint8 fo, oa_toa, dcs, *ud;
	int alphabet = 0;                               /* 0: 7-bit, 1: 8-bit, 2: UCS-2 */
	gboolean udhi;
	gsize hdr = 0;                                  /* UDH octets incl. its length byte */
	p->klass = -1;
	if (len < 2)
		goto bad;
	smsc_len = b[o++];
	if (smsc_len > 0 && o + smsc_len <= len)
		p->smsc = bcd_number(b[o], b + o + 1, (smsc_len - 1) * 2);
	o += smsc_len;
	if (o + 2 > len)
		goto bad;
	fo = b[o++];
	if ((fo & 0x03) != 0x00)                        /* not SMS-DELIVER */
		goto bad;
	udhi = (fo & 0x40) != 0;
	oa_digits = b[o++];
	oa_toa = b[o++];
	if ((oa_toa & 0x70) == 0x50)                    /* alphanumeric sender: 7-bit packed */
		p->number = gsm7_text(b + o, (oa_digits + 1) / 2, 0, oa_digits * 4 / 7);
	else
		p->number = bcd_number(oa_toa, b + o, oa_digits);
	o += (oa_digits + 1) / 2;
	if (o + 10 > len)
		goto bad;
	o++;                                            /* PID */
	dcs = b[o++];
	if ((dcs & 0xC0) == 0x00) {
		alphabet = (dcs >> 2) & 3;
		if (dcs & 0x10)
			p->klass = dcs & 3;
	} else if ((dcs & 0xF0) == 0xF0) {
		alphabet = (dcs & 0x04) ? 1 : 0;
		p->klass = dcs & 3;
	} else if ((dcs & 0xF0) == 0xE0) {
		alphabet = 2;
	}
	{
		const guint8 *t = b + o;
		int tz = bcd2(t[6] & 0xF7), sign = t[6] & 0x08 ? -1 : 1;
		p->timestamp = g_strdup_printf("%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d", 2000 + bcd2(t[0]),
		                               bcd2(t[1]), bcd2(t[2]), bcd2(t[3]), bcd2(t[4]), bcd2(t[5]),
		                               sign < 0 ? '-' : '+', tz * 15 / 60, tz * 15 % 60);
	}
	o += 7;
	udl = b[o++];
	ud = (guint8 *)b + o;
	ud_len = len - o;
	if (udhi && ud_len > 0) {
		gsize i;
		hdr = ud[0] + 1;
		for (i = 1; i + 1 < hdr && i + 1 < ud_len; i += 2 + ud[i + 1]) {
			if (ud[i] == 0x00 && ud[i + 1] == 3 && i + 4 < ud_len) {
				p->ref = ud[i + 2]; p->total = ud[i + 3]; p->seq = ud[i + 4];
			} else if (ud[i] == 0x08 && ud[i + 1] == 4 && i + 5 < ud_len) {
				p->ref = ud[i + 2] << 8 | ud[i + 3]; p->total = ud[i + 4]; p->seq = ud[i + 5];
			}
		}
	}
	if (alphabet == 0) {
		gsize skip = (hdr * 8 + 6) / 7;                 /* header + fill bits, in septets */
		p->text = gsm7_text(ud, ud_len, skip, udl > skip ? udl - skip : 0);
	} else if (alphabet == 2) {
		gsize n = MIN(udl, ud_len);
		p->text = ucs2_text(ud + hdr, n > hdr ? n - hdr : 0);
	} else {
		gsize n = MIN(udl, ud_len);
		p->text = g_strndup((const char *)ud + hdr, n > hdr ? n - hdr : 0);
		if (!g_utf8_validate(p->text, -1, NULL)) {
			g_free(p->text);
			p->text = g_strdup("(binary message)");
		}
	}
	if (p->total < 2)
		p->total = 0;
	return p;
bad:
	sms_part_free(p);
	return NULL;
}

/* SMS-SUBMIT TPDUs for number/text, as uppercase hex */
static GPtrArray *encode_submit(const char *number, const char *text)
{
	GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
	gunichar *u = g_utf8_to_ucs4(text, -1, NULL, NULL, NULL);
	glong n = 0, i;
	gboolean seven = TRUE;
	int *codes, ncodes = 0;                         /* septets (ESC included) for 7-bit */
	GByteArray *da = g_byte_array_new();
	const char *digits = number[0] == '+' ? number + 1 : number;
	gsize nd = strlen(digits);
	static guint8 ref;
	guint parts, p;
	if (!u)
		u = g_new0(gunichar, 1);
	while (u[n])
		n++;
	codes = g_new0(int, 2 * n + 1);
	for (i = 0; i < n; i++) {
		int ext, c = gsm7_code(u[i], &ext);
		if (c < 0) {
			seven = FALSE;
			break;
		}
		if (ext)
			codes[ncodes++] = 0x1B;
		codes[ncodes++] = c;
	}
	/* destination address: digit count, type, BCD */
	g_byte_array_append(da, (guint8[]){ (guint8)nd, number[0] == '+' ? 0x91 : 0x81 }, 2);
	for (i = 0; i < (glong)nd; i += 2) {
		guint8 lo = digits[i] - '0', hi = i + 1 < (glong)nd ? digits[i + 1] - '0' : 0x0F;
		g_byte_array_append(da, (guint8[]){ (guint8)(hi << 4 | lo) }, 1);
	}
	ref++;
	if (seven) {
		int per = ncodes <= 160 ? 160 : 153, start = 0;
		parts = ncodes <= 160 ? 1 : 0;
		if (!parts) {   /* count parts without splitting an ESC pair */
			int s = 0;
			while (s < ncodes) {
				int e = MIN(s + per, ncodes);
				if (e < ncodes && codes[e - 1] == 0x1B)
					e--;
				s = e;
				parts++;
			}
		}
		for (p = 1; p <= parts; p++) {
			int end = MIN(start + per, ncodes), k;
			guint8 ud[160] = { 0 };
			GString *h = g_string_new(NULL);
			gsize hdr = parts > 1 ? 6 : 0, skip = (hdr * 8 + 6) / 7, udl;
			if (end < ncodes && codes[end - 1] == 0x1B)
				end--;
			if (hdr)
				memcpy(ud, (guint8[]){ 5, 0x00, 3, ref, (guint8)parts, (guint8)p }, 6);
			for (k = start; k < end; k++)
				put_septet(ud, skip + (k - start), codes[k]);
			udl = skip + (end - start);
			g_string_append_printf(h, "%02X00", parts > 1 ? 0x41 : 0x01);   /* SUBMIT (+UDHI), MR 0 */
			for (k = 0; k < (int)da->len; k++)
				g_string_append_printf(h, "%02X", da->data[k]);
			g_string_append_printf(h, "0000%02X", (guint)udl);           /* PID, DCS 7-bit, UDL */
			for (k = 0; k < (int)((udl * 7 + 7) / 8); k++)
				g_string_append_printf(h, "%02X", ud[k]);
			g_ptr_array_add(out, g_string_free(h, FALSE));
			start = end;
		}
	} else {
		glong nu16 = 0;
		gunichar2 *w = g_ucs4_to_utf16(u, n, NULL, &nu16, NULL);
		int per = nu16 <= 70 ? 70 : 67, start = 0;
		parts = nu16 <= 70 ? 1 : 0;
		if (!parts) {
			int s = 0;
			while (s < nu16) {
				int e = MIN(s + per, nu16);
				if (e < nu16 && w[e - 1] >= 0xD800 && w[e - 1] <= 0xDBFF)
					e--;                        /* keep surrogate pairs together */
				s = e;
				parts++;
			}
		}
		for (p = 1; p <= parts && w; p++) {
			int end = MIN(start + per, nu16), k;
			GString *h = g_string_new(NULL);
			if (end < nu16 && w[end - 1] >= 0xD800 && w[end - 1] <= 0xDBFF)
				end--;
			g_string_append_printf(h, "%02X00", parts > 1 ? 0x41 : 0x01);
			for (k = 0; k < (int)da->len; k++)
				g_string_append_printf(h, "%02X", da->data[k]);
			g_string_append_printf(h, "0008%02X", (guint)((parts > 1 ? 6 : 0) + 2 * (end - start)));
			if (parts > 1)
				g_string_append_printf(h, "050003%02X%02X%02X", ref, parts, p);
			for (k = start; k < end; k++)
				g_string_append_printf(h, "%04X", w[k]);
			g_ptr_array_add(out, g_string_free(h, FALSE));
			start = end;
		}
		g_free(w);
	}
	g_byte_array_free(da, TRUE);
	g_free(codes);
	g_free(u);
	return out;
}

typedef struct {
	char *path;
	MmGdbusSms *obj;
	GPtrArray *pdus;                 /* to send */
	guint next;
	GDBusMethodInvocation *inv_send;
} Sms;

static MmGdbusModemMessaging *messaging;
static GPtrArray *smss;              /* Sms* */
static guint sms_ids;
static Sms *sending;
static GList *concats;               /* SmsPart* of multipart messages waiting for the rest */

static void messaging_list_changed(void)
{
	GPtrArray *paths = g_ptr_array_new();
	guint i;
	for (i = 0; i < smss->len; i++)
		g_ptr_array_add(paths, ((Sms *)smss->pdata[i])->path);
	g_ptr_array_add(paths, NULL);
	mm_gdbus_modem_messaging_set_messages(messaging, (const gchar *const *)paths->pdata);
	g_ptr_array_free(paths, TRUE);
	g_dbus_interface_skeleton_flush(G_DBUS_INTERFACE_SKELETON(messaging));  /* before Added/Deleted */
}

static gboolean on_sms_send(MmGdbusSms *o, GDBusMethodInvocation *inv, gpointer u);
static gboolean on_sms_store(MmGdbusSms *o, GDBusMethodInvocation *inv, guint storage, gpointer u)
{
	mm_gdbus_sms_complete_store(o, inv);             /* they are kept in memory anyway */
	return TRUE;
}

static Sms *sms_new(void)
{
	Sms *s = g_new0(Sms, 1);
	GError *err = NULL;
	s->path = g_strdup_printf(SMS_PATH "%u", sms_ids++);
	s->obj = mm_gdbus_sms_skeleton_new();
	mm_gdbus_sms_set_validity(s->obj, g_variant_new("(uv)", (guint)MM_SMS_VALIDITY_TYPE_UNKNOWN,
	                                                 g_variant_new_uint32(0)));
	mm_gdbus_sms_set_class(s->obj, -1);
	mm_gdbus_sms_set_data(s->obj, g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, "", 0, 1));
	mm_gdbus_sms_set_delivery_state(s->obj, MM_SMS_DELIVERY_STATE_UNKNOWN);
	mm_gdbus_sms_set_storage(s->obj, MM_SMS_STORAGE_ME);
	mm_gdbus_sms_set_timestamp(s->obj, "");
	mm_gdbus_sms_set_discharge_timestamp(s->obj, "");
	mm_gdbus_sms_set_smsc(s->obj, "");
	g_signal_connect(s->obj, "handle-send", G_CALLBACK(on_sms_send), s);
	g_signal_connect(s->obj, "handle-store", G_CALLBACK(on_sms_store), s);
	if (!g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(s->obj), bus, s->path, &err)) {
		say("sms: export failed: %s", err->message);
		g_error_free(err);
	}
	g_ptr_array_add(smss, s);
	messaging_list_changed();
	return s;
}

static void sms_free(Sms *s)
{
	g_dbus_interface_skeleton_unexport(G_DBUS_INTERFACE_SKELETON(s->obj));
	g_object_unref(s->obj);
	if (s->pdus)
		g_ptr_array_free(s->pdus, TRUE);
	g_free(s->path);
	g_free(s);
}

static void sms_received(SmsPart *p)
{
	Sms *s = sms_new();
	mm_gdbus_sms_set_state(s->obj, MM_SMS_STATE_RECEIVED);
	mm_gdbus_sms_set_pdu_type(s->obj, MM_SMS_PDU_TYPE_DELIVER);
	mm_gdbus_sms_set_number(s->obj, p->number ? p->number : "");
	mm_gdbus_sms_set_text(s->obj, p->text ? p->text : "");
	mm_gdbus_sms_set_smsc(s->obj, p->smsc ? p->smsc : "");
	mm_gdbus_sms_set_timestamp(s->obj, p->timestamp ? p->timestamp : "");
	mm_gdbus_sms_set_class(s->obj, p->klass);
	mm_gdbus_sms_set_storage(s->obj, MM_SMS_STORAGE_MT);
	mm_gdbus_modem_messaging_emit_added(messaging, s->path, TRUE);
	say("sms: received %u character(s) (%s)", (guint)g_utf8_strlen(p->text ? p->text : "", -1),
	    s->path);
}

/* one PDU in: single -> published; a part -> kept until the others are there */
static void sms_part_in(SmsPart *p)
{
	GList *l;
	SmsPart **all;
	guint have = 0, i;
	if (!p->total) {
		sms_received(p);
		sms_part_free(p);
		return;
	}
	concats = g_list_append(concats, p);
	all = g_new0(SmsPart *, p->total + 1);
	for (l = concats; l; l = l->next) {
		SmsPart *q = l->data;
		if (q->ref == p->ref && q->total == p->total && !g_strcmp0(q->number, p->number) &&
		    q->seq >= 1 && q->seq <= p->total && !all[q->seq - 1]) {
			all[q->seq - 1] = q;
			have++;
		}
	}
	if (have == p->total) {
		GString *t = g_string_new(NULL);
		SmsPart joined = *all[0];
		for (i = 0; i < p->total; i++)
			g_string_append(t, all[i]->text ? all[i]->text : "");
		joined.text = t->str;
		joined.timestamp = all[p->total - 1]->timestamp;
		sms_received(&joined);
		g_string_free(t, TRUE);
		for (i = 0; i < p->total; i++) {
			concats = g_list_remove(concats, all[i]);
			sms_part_free(all[i]);
		}
	} else {
		say("sms: part %u of %u", p->seq, p->total);
	}
	g_free(all);
}

static void ack_incoming_sms(void)
{
	GBinderWriter w;
	GBinderLocalRequest *r;
	if (!client[MSG])
		return;
	r = req_new(MSG, &w);
	gbinder_writer_append_int32(&w, next_serial++);
	gbinder_writer_append_bool(&w, TRUE);           /* success */
	gbinder_writer_append_int32(&w, 0);             /* cause (only for failures) */
	send_req(MSG, MSG_acknowledgeLastIncomingGsmSms, r);
}

static void send_next_part(void)
{
	GBinderWriter w, m;
	GBinderLocalRequest *r;
	gboolean more = sending->next + 1 < sending->pdus->len;
	r = req_new(MSG, &w);
	gbinder_writer_append_int32(&w, next_serial++);
	gbinder_writer_start_parcelable(&w, &m);        /* GsmSmsMessage */
	gbinder_writer_append_string16(&m, "");         /*   smscPdu: the SIM's SMSC */
	gbinder_writer_append_string16(&m, sending->pdus->pdata[sending->next]);
	gbinder_writer_finish_parcelable(&m);
	send_req(MSG, more ? MSG_sendSmsExpectMore : MSG_sendSms, r);
}

static void sms_send_done(const char *error)
{
	Sms *s = sending;
	sending = NULL;
	if (!s)
		return;
	if (error) {
		say("sms: sending failed: %s", error);
		mm_gdbus_sms_set_state(s->obj, MM_SMS_STATE_UNKNOWN);
		if (s->inv_send)
			g_dbus_method_invocation_return_error(s->inv_send, MM_CORE_ERROR, MM_CORE_ERROR_FAILED,
			                                      "sending failed: %s", error);
	} else {
		say("sms: sent (%u part(s))", s->pdus->len);
		mm_gdbus_sms_set_state(s->obj, MM_SMS_STATE_SENT);
		if (s->inv_send)
			mm_gdbus_sms_complete_send(s->obj, s->inv_send);
	}
	s->inv_send = NULL;
}

static gboolean on_sms_send(MmGdbusSms *o, GDBusMethodInvocation *inv, gpointer u)
{
	Sms *s = u;
	const char *number = mm_gdbus_sms_get_number(o), *text = mm_gdbus_sms_get_text(o);
	if (!client[MSG] || (!registered(st.voice_reg) && !registered(st.data_reg))) {
		g_dbus_method_invocation_return_error(inv, MM_CORE_ERROR, MM_CORE_ERROR_WRONG_STATE,
		                                      "not registered on a network");
		return TRUE;
	}
	if (sending) {
		g_dbus_method_invocation_return_error(inv, MM_CORE_ERROR, MM_CORE_ERROR_IN_PROGRESS,
		                                      "another message is being sent");
		return TRUE;
	}
	if (!number || !*number || !text || !g_regex_match_simple("^\\+?[0-9]{1,20}$", number, 0, 0)) {
		g_dbus_method_invocation_return_error(inv, MM_CORE_ERROR, MM_CORE_ERROR_INVALID_ARGS,
		                                      "needs a phone number and text");
		return TRUE;
	}
	if (s->pdus)
		g_ptr_array_free(s->pdus, TRUE);
	s->pdus = encode_submit(number, text);
	s->next = 0;
	s->inv_send = inv;
	sending = s;
	mm_gdbus_sms_set_state(o, MM_SMS_STATE_SENDING);
	say("sms: sending %u part(s)", s->pdus->len);
	send_next_part();
	return TRUE;
}

static gboolean on_messaging_list(MmGdbusModemMessaging *o, GDBusMethodInvocation *inv, gpointer u)
{
	GPtrArray *paths = g_ptr_array_new();
	guint i;
	for (i = 0; i < smss->len; i++)
		g_ptr_array_add(paths, ((Sms *)smss->pdata[i])->path);
	g_ptr_array_add(paths, NULL);
	mm_gdbus_modem_messaging_complete_list(o, inv, (const gchar *const *)paths->pdata);
	g_ptr_array_free(paths, TRUE);
	return TRUE;
}

static gboolean on_messaging_delete(MmGdbusModemMessaging *o, GDBusMethodInvocation *inv,
                                    const gchar *path, gpointer u)
{
	guint i;
	for (i = 0; i < smss->len; i++) {
		Sms *s = smss->pdata[i];
		if (!strcmp(s->path, path)) {
			if (s == sending) {
				g_dbus_method_invocation_return_error(inv, MM_CORE_ERROR, MM_CORE_ERROR_IN_PROGRESS,
				                                      "the message is being sent");
				return TRUE;
			}
			g_ptr_array_remove_index(smss, i);
			messaging_list_changed();
			mm_gdbus_modem_messaging_emit_deleted(o, path);
			sms_free(s);
			mm_gdbus_modem_messaging_complete_delete(o, inv);
			return TRUE;
		}
	}
	g_dbus_method_invocation_return_error(inv, MM_CORE_ERROR, MM_CORE_ERROR_NOT_FOUND, "no such message");
	return TRUE;
}

static gboolean on_messaging_create(MmGdbusModemMessaging *o, GDBusMethodInvocation *inv,
                                    GVariant *props, gpointer u)
{
	const char *number = NULL, *text = NULL;
	Sms *s;
	g_variant_lookup(props, "number", "&s", &number);
	g_variant_lookup(props, "text", "&s", &text);
	if (!number || !*number || !text) {
		g_dbus_method_invocation_return_error(inv, MM_CORE_ERROR, MM_CORE_ERROR_INVALID_ARGS,
		                                      "a text message needs 'number' and 'text'");
		return TRUE;
	}
	s = sms_new();
	mm_gdbus_sms_set_state(s->obj, MM_SMS_STATE_UNKNOWN);
	mm_gdbus_sms_set_pdu_type(s->obj, MM_SMS_PDU_TYPE_SUBMIT);
	mm_gdbus_sms_set_number(s->obj, number);
	mm_gdbus_sms_set_text(s->obj, text);
	mm_gdbus_sms_set_storage(s->obj, MM_SMS_STORAGE_UNKNOWN);
	mm_gdbus_modem_messaging_emit_added(o, s->path, FALSE);
	mm_gdbus_modem_messaging_complete_create(o, inv, s->path);
	return TRUE;
}

static gboolean on_messaging_storage(MmGdbusModemMessaging *o, GDBusMethodInvocation *inv, guint st_,
                                     gpointer u)
{
	mm_gdbus_modem_messaging_complete_set_default_storage(o, inv);
	return TRUE;
}

static void messaging_setup(void)
{
	GVariantBuilder b;
	smss = g_ptr_array_new();
	messaging = mm_gdbus_modem_messaging_skeleton_new();
	g_variant_builder_init(&b, G_VARIANT_TYPE("au"));
	g_variant_builder_add(&b, "u", (guint)MM_SMS_STORAGE_ME);
	mm_gdbus_modem_messaging_set_supported_storages(messaging, g_variant_builder_end(&b));
	mm_gdbus_modem_messaging_set_default_storage(messaging, MM_SMS_STORAGE_ME);
	messaging_list_changed();
	g_signal_connect(messaging, "handle-list", G_CALLBACK(on_messaging_list), NULL);
	g_signal_connect(messaging, "handle-delete", G_CALLBACK(on_messaging_delete), NULL);
	g_signal_connect(messaging, "handle-create", G_CALLBACK(on_messaging_create), NULL);
	g_signal_connect(messaging, "handle-set-default-storage", G_CALLBACK(on_messaging_storage), NULL);
}

/* ---------- calls ------------------------------------------------------------------------------
 * radio.voice v2 <-> ModemManager's Modem.Voice and Call objects (what GNOME Calls and Plasma
 * Dialer use). As in Android's phone service, the modem's call list is the truth: every change it
 * reports (and a 1 s poll while there are calls) asks for the list again, and the Call objects
 * follow it by the modem's call index. Emergency numbers go out as emergency calls, as Android
 * does it (emergencyDial). */

#define CALL_PATH MM_PATH "/Call/"

typedef struct {
	char *path;
	MmGdbusCall *obj;
	int index;                      /* the modem's index for the call; 0 until it is listed */
	gboolean started;               /* outgoing: dial sent */
	gboolean dialed;                /* outgoing: the modem took the dial */
	gboolean hung_up;               /* we asked the modem to end it */
	int misses;                     /* call lists without it since it was dialed */
	guint reaper;                   /* removes it a while after it ended */
	GString *dtmf;                  /* tones still to send */
	GDBusMethodInvocation *inv_dtmf;
} Call;

typedef struct { int state, index, toa, mpty, mt, pres; char *number; gboolean used; } ModemCall;

static GPtrArray *calls;             /* Call* */
static guint call_ids;
static gboolean clcc_busy, clcc_again;
static guint clcc_timer;
static char **ecc;                   /* the modem's emergency numbers */
/* one modem request for a Call or Voice method at a time */
static GDBusMethodInvocation *vop_inv;
static guint32 vop_code;             /* the response that completes it */
static Call *vop_call;
static Call *dtmf_call;              /* sending tones */

static void call_audio_update(void);

static Call *call_by_path(const char *path)
{
	guint i;
	for (i = 0; calls && i < calls->len; i++)
		if (!strcmp(((Call *)calls->pdata[i])->path, path))
			return calls->pdata[i];
	return NULL;
}

static gboolean call_live(Call *c)
{
	MMCallState s = mm_gdbus_call_get_state(c->obj);
	return s != MM_CALL_STATE_TERMINATED;
}

static void voice_list_changed(void)
{
	GPtrArray *paths = g_ptr_array_new();
	guint i;
	for (i = 0; i < calls->len; i++)
		g_ptr_array_add(paths, ((Call *)calls->pdata[i])->path);
	g_ptr_array_add(paths, NULL);
	mm_gdbus_modem_voice_set_calls(voice, (const gchar *const *)paths->pdata);
	g_ptr_array_free(paths, TRUE);
	/* out now, before CallAdded/CallDeleted, as ModemManager does: libmm-glib's ListCalls reads the
	 * cached Calls property, and GNOME Calls only starts a call it finds there */
	g_dbus_interface_skeleton_flush(G_DBUS_INTERFACE_SKELETON(voice));
}

static const char *call_state_name(MMCallState s)
{
	switch (s) {
	case MM_CALL_STATE_DIALING: return "dialing";
	case MM_CALL_STATE_RINGING_OUT: return "ringing out";
	case MM_CALL_STATE_RINGING_IN: return "ringing in";
	case MM_CALL_STATE_ACTIVE: return "active";
	case MM_CALL_STATE_HELD: return "held";
	case MM_CALL_STATE_WAITING: return "waiting";
	case MM_CALL_STATE_TERMINATED: return "ended";
	default: return "new";
	}
}

static void call_remove(Call *c)
{
	if (c == vop_call)
		vop_call = NULL;
	if (c == dtmf_call)
		dtmf_call = NULL;
	if (c->reaper)
		g_source_remove(c->reaper);
	if (c->inv_dtmf)
		g_dbus_method_invocation_return_error(c->inv_dtmf, MM_CORE_ERROR, MM_CORE_ERROR_ABORTED,
		                                      "the call is gone");
	g_ptr_array_remove(calls, c);
	voice_list_changed();
	mm_gdbus_modem_voice_emit_call_deleted(voice, c->path);
	g_dbus_interface_skeleton_unexport(G_DBUS_INTERFACE_SKELETON(c->obj));
	g_object_unref(c->obj);
	if (c->dtmf)
		g_string_free(c->dtmf, TRUE);
	g_free(c->path);
	g_free(c);
}

static gboolean call_reap(gpointer data)    /* ended calls go away by themselves, as in ModemManager */
{
	Call *c = data;
	c->reaper = 0;
	call_remove(c);
	return G_SOURCE_REMOVE;
}

static gboolean call_unstarted(gpointer data)    /* created a minute ago and never started */
{
	Call *c = data;
	c->reaper = 0;
	say("call: %s was never started, dropped", c->path);
	call_remove(c);
	return G_SOURCE_REMOVE;
}

static void call_set_state(Call *c, MMCallState s, MMCallStateReason why)
{
	MMCallState old = mm_gdbus_call_get_state(c->obj);
	if (old == s)
		return;
	mm_gdbus_call_set_state(c->obj, s);
	mm_gdbus_call_set_state_reason(c->obj, why);
	g_dbus_interface_skeleton_flush(G_DBUS_INTERFACE_SKELETON(c->obj));  /* cache agrees with the signal */
	mm_gdbus_call_emit_state_changed(c->obj, old, s, why);
	say("call: %s %s -> %s", c->path + strlen(CALL_PATH) - 5, call_state_name(old), call_state_name(s));
	if (s == MM_CALL_STATE_TERMINATED) {
		if (c->inv_dtmf) {
			g_dbus_method_invocation_return_error(c->inv_dtmf, MM_CORE_ERROR, MM_CORE_ERROR_ABORTED,
			                                      "the call ended");
			c->inv_dtmf = NULL;
		}
		if (c->reaper)                              /* the never-started timer */
			g_source_remove(c->reaper);
		c->reaper = g_timeout_add_seconds(5, call_reap, c);
	}
	call_audio_update();
}

static gboolean on_call_start(MmGdbusCall *o, GDBusMethodInvocation *inv, gpointer u);
static gboolean on_call_accept(MmGdbusCall *o, GDBusMethodInvocation *inv, gpointer u);
static gboolean on_call_hangup(MmGdbusCall *o, GDBusMethodInvocation *inv, gpointer u);
static gboolean on_call_dtmf(MmGdbusCall *o, GDBusMethodInvocation *inv, const gchar *tones, gpointer u);
static gboolean on_call_join(MmGdbusCall *o, GDBusMethodInvocation *inv, gpointer u);
static gboolean on_call_leave(MmGdbusCall *o, GDBusMethodInvocation *inv, gpointer u);

static Call *call_new(MMCallDirection dir, const char *number, MMCallState s, MMCallStateReason why)
{
	Call *c = g_new0(Call, 1);
	GError *err = NULL;
	c->path = g_strdup_printf(CALL_PATH "%u", call_ids++);
	c->obj = mm_gdbus_call_skeleton_new();
	mm_gdbus_call_set_direction(c->obj, dir);
	mm_gdbus_call_set_number(c->obj, number ? number : "");
	mm_gdbus_call_set_state(c->obj, s);
	mm_gdbus_call_set_state_reason(c->obj, why);
	mm_gdbus_call_set_multiparty(c->obj, FALSE);
	mm_gdbus_call_set_audio_port(c->obj, "");
	mm_gdbus_call_set_audio_format(c->obj, g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0));
	g_signal_connect(c->obj, "handle-start", G_CALLBACK(on_call_start), c);
	g_signal_connect(c->obj, "handle-accept", G_CALLBACK(on_call_accept), c);
	g_signal_connect(c->obj, "handle-hangup", G_CALLBACK(on_call_hangup), c);
	g_signal_connect(c->obj, "handle-send-dtmf", G_CALLBACK(on_call_dtmf), c);
	g_signal_connect(c->obj, "handle-join-multiparty", G_CALLBACK(on_call_join), c);
	g_signal_connect(c->obj, "handle-leave-multiparty", G_CALLBACK(on_call_leave), c);
	g_signal_connect(c->obj, "handle-deflect", G_CALLBACK(on_unsupported), NULL);
	if (!g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(c->obj), bus, c->path, &err)) {
		say("call: export failed: %s", err->message);
		g_error_free(err);
	}
	g_ptr_array_add(calls, c);
	voice_list_changed();
	mm_gdbus_modem_voice_emit_call_added(voice, c->path);
	return c;
}

static gboolean is_emergency(const char *number)
{
	int i;
	if (!g_strcmp0(number, "112") || !g_strcmp0(number, "911"))
		return TRUE;
	for (i = 0; ecc && ecc[i]; i++)
		if (!strcmp(ecc[i], number))
			return TRUE;
	return FALSE;
}

static void write_dial(GBinderWriter *w, const char *number)   /* Dial */
{
	GBinderWriter d;
	gbinder_writer_start_parcelable(w, &d);
	gbinder_writer_append_string16(&d, number);
	gbinder_writer_append_int32(&d, 0);             /* clir: CLIR_DEFAULT (the subscription's) */
	gbinder_writer_append_int32(&d, 0);             /* uusInfo: none */
	gbinder_writer_finish_parcelable(&d);
}

static void voice_call_index(guint32 code, int index)  /* hangup, separateConnection */
{
	GBinderWriter w;
	GBinderLocalRequest *r = req_new(VOICE, &w);
	gbinder_writer_append_int32(&w, next_serial++);
	gbinder_writer_append_int32(&w, index);
	send_req(VOICE, code, r);
}

static void clcc(void)    /* ask for the modem's call list */
{
	if (!client[VOICE])
		return;
	if (clcc_busy) {
		clcc_again = TRUE;
		return;
	}
	clcc_busy = TRUE;
	call_serial(VOICE, VOICE_getCurrentCalls);
}

static gboolean clcc_poll(gpointer unused)
{
	guint i;
	gboolean any = FALSE;
	for (i = 0; i < calls->len; i++)
		if (call_live(calls->pdata[i]))
			any = TRUE;
	if (!any || !st.up) {
		clcc_timer = 0;
		return G_SOURCE_REMOVE;
	}
	clcc();
	return G_SOURCE_CONTINUE;
}

static void clcc_poll_start(void)
{
	if (!clcc_timer)
		clcc_timer = g_timeout_add_seconds(1, clcc_poll, NULL);
}

static MMCallState mm_call_state(int ril)
{
	switch (ril) {
	case 0: return MM_CALL_STATE_ACTIVE;
	case 1: return MM_CALL_STATE_HELD;
	case 2: return MM_CALL_STATE_DIALING;
	case 3: return MM_CALL_STATE_RINGING_OUT;
	case 4: return MM_CALL_STATE_RINGING_IN;
	case 5: return MM_CALL_STATE_WAITING;
	}
	return MM_CALL_STATE_UNKNOWN;
}

static char *call_number(const ModemCall *m)
{
	if (m->pres == 1 || m->pres == 2 || !m->number || !*m->number)  /* withheld / unknown */
		return g_strdup("");
	if (m->toa == 145 && m->number[0] != '+')       /* international */
		return g_strconcat("+", m->number, NULL);
	return g_strdup(m->number);
}

static void read_call_list(GBinderReader *r)
{
	GArray *list = g_array_new(FALSE, TRUE, sizeof(ModemCall));
	gint32 n = 0, i, v;
	guint j;
	gbinder_reader_read_int32(r, &n);
	for (i = 0; i < n && i < 16; i++) {
		GBinderReader p;
		gboolean nn;
		ModemCall m = { 0 };
		if (!gbinder_reader_start_parcelable(r, &p, &nn) || !nn)
			break;
		gbinder_reader_read_int32(&p, &m.state);
		gbinder_reader_read_int32(&p, &m.index);
		gbinder_reader_read_int32(&p, &m.toa);
		gbinder_reader_read_int32(&p, &m.mpty);
		gbinder_reader_read_int32(&p, &m.mt);
		gbinder_reader_read_int32(&p, &v);          /* als */
		gbinder_reader_read_int32(&p, &v);          /* isVoice */
		gbinder_reader_read_int32(&p, &v);          /* isVoicePrivacy */
		m.number = gbinder_reader_read_string16(&p);
		gbinder_reader_read_int32(&p, &m.pres);
		gbinder_reader_finish_parcelable(&p);
		g_array_append_val(list, m);
	}
	/* calls we know: follow their entry, or they ended */
	for (j = 0; j < calls->len; j++) {
		Call *c = calls->pdata[j];
		ModemCall *hit = NULL;
		if (!call_live(c) || (!c->index && !c->started &&
		                      mm_gdbus_call_get_direction(c->obj) == MM_CALL_DIRECTION_OUTGOING))
			continue;                               /* ended, or created and not started yet */
		for (i = 0; i < (gint32)list->len && !hit; i++) {
			ModemCall *m = &g_array_index(list, ModemCall, i);
			if (!m->used && c->index && m->index == c->index)
				hit = m;
		}
		if (!hit && !c->index) {                    /* a call we dialed: the new outgoing entry */
			for (i = 0; i < (gint32)list->len && !hit; i++) {
				ModemCall *m = &g_array_index(list, ModemCall, i);
				if (!m->used && !m->mt && (m->state == 2 || m->state == 3 || m->state == 0)) {
					gboolean known = FALSE;
					guint k;
					for (k = 0; k < calls->len; k++)
						if (((Call *)calls->pdata[k])->index == m->index)
							known = TRUE;
					if (!known)
						hit = m;
				}
			}
			if (hit)
				c->index = hit->index;
		}
		if (hit) {
			MMCallState s = mm_call_state(hit->state);
			hit->used = TRUE;
			mm_gdbus_call_set_multiparty(c->obj, hit->mpty);
			if (s != MM_CALL_STATE_UNKNOWN)
				call_set_state(c, s, s == MM_CALL_STATE_ACTIVE &&
				               mm_gdbus_call_get_direction(c->obj) == MM_CALL_DIRECTION_INCOMING ?
				               MM_CALL_STATE_REASON_ACCEPTED : MM_CALL_STATE_REASON_OUTGOING_STARTED);
		} else if (c->index || (c->dialed && ++c->misses >= 3)) {
			if (!c->hung_up)
				call_serial(VOICE, VOICE_getLastCallFailCause);  /* why, for the log */
			call_set_state(c, MM_CALL_STATE_TERMINATED, MM_CALL_STATE_REASON_TERMINATED);
		}
	}
	/* new entries: incoming (or waiting) calls, or calls started elsewhere (SIM toolkit) */
	for (i = 0; i < (gint32)list->len; i++) {
		ModemCall *m = &g_array_index(list, ModemCall, i);
		MMCallState s = mm_call_state(m->state);
		gboolean known = FALSE;
		for (j = 0; j < calls->len; j++)
			if (call_live(calls->pdata[j]) && ((Call *)calls->pdata[j])->index == m->index)
				known = TRUE;
		if (!m->used && !known && s != MM_CALL_STATE_UNKNOWN) {
			char *num = call_number(m);
			Call *c = call_new(m->mt ? MM_CALL_DIRECTION_INCOMING : MM_CALL_DIRECTION_OUTGOING, num,
			                   s, m->mt ? MM_CALL_STATE_REASON_INCOMING_NEW :
			                   MM_CALL_STATE_REASON_OUTGOING_STARTED);
			c->index = m->index;
			say("call: %s %s %s", m->mt ? "incoming" : "outgoing (started by the modem)", c->path,
			    *num ? "(number known)" : "(number withheld)");
			g_free(num);
			call_audio_update();
		}
	}
	for (i = 0; i < (gint32)list->len; i++)
		g_free(g_array_index(list, ModemCall, i).number);
	g_array_free(list, TRUE);
	clcc_poll_start();
}

static void vop_done(int error)       /* the modem answered the pending Call/Voice request */
{
	GDBusMethodInvocation *inv = vop_inv;
	Call *c = vop_call;
	guint32 code = vop_code;
	vop_inv = NULL;
	vop_call = NULL;
	vop_code = 0;
	if (code == VOICER_dial || code == VOICER_emergencyDial) {
		if (c && !error) {
			c->dialed = TRUE;
			if (mm_gdbus_call_get_state(c->obj) == MM_CALL_STATE_UNKNOWN)  /* the list may be ahead */
				call_set_state(c, MM_CALL_STATE_DIALING, MM_CALL_STATE_REASON_OUTGOING_STARTED);
		} else if (c) {
			say("call: the modem refused the dial: %s", radio_err(error));
			call_set_state(c, MM_CALL_STATE_TERMINATED, MM_CALL_STATE_REASON_ERROR);
		}
	}
	if (inv) {
		if (error)
			g_dbus_method_invocation_return_error(inv, MM_CORE_ERROR, MM_CORE_ERROR_FAILED, "%s",
			                                      radio_err(error));
		else
			g_dbus_method_invocation_return_value(inv, NULL);   /* all of these return nothing */
	}
	clcc();
}

static gboolean vop_begin(GDBusMethodInvocation *inv, Call *c, guint32 reply)
{
	if (!client[VOICE] || !st.up) {
		g_dbus_method_invocation_return_error(inv, MM_CORE_ERROR, MM_CORE_ERROR_WRONG_STATE,
		                                      "the modem has no voice service");
		return FALSE;
	}
	if (vop_inv) {
		g_dbus_method_invocation_return_error(inv, MM_CORE_ERROR, MM_CORE_ERROR_IN_PROGRESS,
		                                      "the modem is busy with another call request");
		return FALSE;
	}
	vop_inv = inv;
	vop_call = c;
	vop_code = reply;
	return TRUE;
}

static gboolean on_call_start(MmGdbusCall *o, GDBusMethodInvocation *inv, gpointer u)
{
	Call *c = u;
	const char *number = mm_gdbus_call_get_number(o);
	gboolean sos = is_emergency(number);
	GBinderWriter w;
	GBinderLocalRequest *r;
	if (c->started || mm_gdbus_call_get_state(o) != MM_CALL_STATE_UNKNOWN ||
	    mm_gdbus_call_get_direction(o) != MM_CALL_DIRECTION_OUTGOING) {
		g_dbus_method_invocation_return_error(inv, MM_CORE_ERROR, MM_CORE_ERROR_WRONG_STATE,
		                                      "the call was already started");
		return TRUE;
	}
	if (c->reaper) {                                /* the never-started timer */
		g_source_remove(c->reaper);
		c->reaper = 0;
	}
	/* a start that fails ends the call, as in ModemManager: GNOME Calls dials nothing new while
	 * it still has a call */
	if (!sos && !registered(st.voice_reg)) {
		say("call: %s not dialed: not registered for calls (voice registration %d)", c->path,
		    st.voice_reg);
		g_dbus_method_invocation_return_error(inv, MM_CORE_ERROR, MM_CORE_ERROR_WRONG_STATE,
		                                      "not registered for calls");
		call_set_state(c, MM_CALL_STATE_TERMINATED, MM_CALL_STATE_REASON_ERROR);
		return TRUE;
	}
	if (!vop_begin(inv, c, sos ? VOICER_emergencyDial : VOICER_dial)) {
		say("call: %s not dialed: the modem is busy or has no voice service", c->path);
		call_set_state(c, MM_CALL_STATE_TERMINATED, MM_CALL_STATE_REASON_ERROR);
		return TRUE;
	}
	c->started = TRUE;
	call_set_state(c, MM_CALL_STATE_DIALING, MM_CALL_STATE_REASON_OUTGOING_STARTED);
	r = req_new(VOICE, &w);
	gbinder_writer_append_int32(&w, next_serial++);
	write_dial(&w, number);
	if (sos) {
		gbinder_writer_append_int32(&w, 0);         /* categories: unspecified */
		gbinder_writer_append_int32(&w, 0);         /* urns: none */
		gbinder_writer_append_int32(&w, 0);         /* routing: UNKNOWN, the modem decides */
		gbinder_writer_append_bool(&w, TRUE);       /* hasKnownUserIntentEmergency */
		gbinder_writer_append_bool(&w, FALSE);      /* isTesting */
	}
	say("call: %s %s", sos ? "emergency call" : "dialing", c->path);
	send_req(VOICE, sos ? VOICE_emergencyDial : VOICE_dial, r);
	call_audio_update();                            /* audio comes up with the dial, as on Android */
	clcc_poll_start();
	return TRUE;
}

static gboolean on_call_accept(MmGdbusCall *o, GDBusMethodInvocation *inv, gpointer u)
{
	Call *c = u;
	MMCallState s = mm_gdbus_call_get_state(o);
	if (s != MM_CALL_STATE_RINGING_IN && s != MM_CALL_STATE_WAITING) {
		g_dbus_method_invocation_return_error(inv, MM_CORE_ERROR, MM_CORE_ERROR_WRONG_STATE,
		                                      "the call isn't ringing");
		return TRUE;
	}
	if (s == MM_CALL_STATE_RINGING_IN) {
		if (vop_begin(inv, c, VOICER_acceptCall)) {
			say("call: answering %s", c->path);
			call_serial(VOICE, VOICE_acceptCall);
		}
	} else if (vop_begin(inv, c, VOICER_switchWaitingOrHoldingAndActive)) {
		say("call: answering waiting %s (the other is held)", c->path);
		call_serial(VOICE, VOICE_switchWaitingOrHoldingAndActive);
	}
	return TRUE;
}

static gboolean on_call_hangup(MmGdbusCall *o, GDBusMethodInvocation *inv, gpointer u)
{
	Call *c = u;
	if (!call_live(c)) {
		g_dbus_method_invocation_return_value(inv, NULL);
		return TRUE;
	}
	if (!c->index && !c->started) {                 /* never dialed */
		call_set_state(c, MM_CALL_STATE_TERMINATED, MM_CALL_STATE_REASON_TERMINATED);
		g_dbus_method_invocation_return_value(inv, NULL);
		return TRUE;
	}
	if (!vop_begin(inv, c, c->index ? VOICER_hangupConnection :
	               VOICER_hangupForegroundResumeBackground))
		return TRUE;
	c->hung_up = TRUE;
	say("call: hanging up %s", c->path);
	if (c->index)
		voice_call_index(VOICE_hangup, c->index);
	else                                            /* dialing, not listed yet: the foreground call */
		call_serial(VOICE, VOICE_hangupForegroundResumeBackground);
	return TRUE;
}

static void dtmf_next(void)
{
	Call *c = dtmf_call;
	char tone[2] = { 0, 0 };
	GBinderWriter w;
	GBinderLocalRequest *r;
	if (!c)
		return;
	if (!c->dtmf || !c->dtmf->len || !call_live(c)) {
		if (c->inv_dtmf)
			mm_gdbus_call_complete_send_dtmf(c->obj, c->inv_dtmf);
		c->inv_dtmf = NULL;
		dtmf_call = NULL;
		return;
	}
	tone[0] = c->dtmf->str[0];
	g_string_erase(c->dtmf, 0, 1);
	r = req_new(VOICE, &w);
	gbinder_writer_append_int32(&w, next_serial++);
	gbinder_writer_append_string16(&w, tone);
	send_req(VOICE, VOICE_sendDtmf, r);
}

static gboolean on_call_dtmf(MmGdbusCall *o, GDBusMethodInvocation *inv, const gchar *tones, gpointer u)
{
	Call *c = u;
	if (mm_gdbus_call_get_state(o) != MM_CALL_STATE_ACTIVE) {
		g_dbus_method_invocation_return_error(inv, MM_CORE_ERROR, MM_CORE_ERROR_WRONG_STATE,
		                                      "tones need an active call");
		return TRUE;
	}
	if (!tones || !g_regex_match_simple("^[0-9A-Da-d*#]+$", tones, 0, 0)) {
		g_dbus_method_invocation_return_error(inv, MM_CORE_ERROR, MM_CORE_ERROR_INVALID_ARGS,
		                                      "tones are 0-9, A-D, * and #");
		return TRUE;
	}
	if (dtmf_call) {
		g_dbus_method_invocation_return_error(inv, MM_CORE_ERROR, MM_CORE_ERROR_IN_PROGRESS,
		                                      "still sending tones");
		return TRUE;
	}
	if (!c->dtmf)
		c->dtmf = g_string_new(NULL);
	g_string_assign(c->dtmf, tones);
	g_string_ascii_up(c->dtmf);
	c->inv_dtmf = inv;
	dtmf_call = c;
	dtmf_next();
	return TRUE;
}

static gboolean on_call_join(MmGdbusCall *o, GDBusMethodInvocation *inv, gpointer u)
{
	if (vop_begin(inv, u, VOICER_conference))
		call_serial(VOICE, VOICE_conference);
	return TRUE;
}

static gboolean on_call_leave(MmGdbusCall *o, GDBusMethodInvocation *inv, gpointer u)
{
	Call *c = u;
	if (!c->index) {
		g_dbus_method_invocation_return_error(inv, MM_CORE_ERROR, MM_CORE_ERROR_WRONG_STATE,
		                                      "the call isn't listed by the modem");
		return TRUE;
	}
	if (vop_begin(inv, c, VOICER_separateConnection))
		voice_call_index(VOICE_separateConnection, c->index);
	return TRUE;
}

static gboolean on_voice_create(MmGdbusModemVoice *o, GDBusMethodInvocation *inv, GVariant *props,
                                gpointer u)
{
	const char *number = NULL;
	char *num = NULL, *s, *d;
	Call *c;
	if (g_variant_lookup(props, "number", "&s", &number) && number) {
		num = g_strdup(number);
		for (s = d = num; *s; s++)                  /* separators address books put in numbers */
			if (!strchr(" -().\t/", *s))
				*d++ = *s;
		*d = 0;
	}
	if (!num || !g_regex_match_simple("^\\+?[0-9*#]{1,40}$", num, 0, 0)) {
		g_free(num);
		g_dbus_method_invocation_return_error(inv, MM_CORE_ERROR, MM_CORE_ERROR_INVALID_ARGS,
		                                      "a call needs a 'number' (digits, * and #)");
		return TRUE;
	}
	c = call_new(MM_CALL_DIRECTION_OUTGOING, num, MM_CALL_STATE_UNKNOWN,
	             MM_CALL_STATE_REASON_UNKNOWN);
	g_free(num);
	/* GNOME Calls starts any outgoing call it finds still unstarted, also one left over from before
	 * it (re)started: drop the ones nobody starts, so nothing gets dialed out of the blue later */
	c->reaper = g_timeout_add_seconds(60, call_unstarted, c);
	say("call: created %s", c->path);
	mm_gdbus_modem_voice_complete_create_call(o, inv, c->path);
	return TRUE;
}

static gboolean on_voice_delete(MmGdbusModemVoice *o, GDBusMethodInvocation *inv, const gchar *path,
                                gpointer u)
{
	Call *c = call_by_path(path);
	if (!c) {
		g_dbus_method_invocation_return_error(inv, MM_CORE_ERROR, MM_CORE_ERROR_NOT_FOUND, "no such call");
		return TRUE;
	}
	if (call_live(c) && (c->index || c->started)) {  /* "the call will be hung up if still active" */
		c->hung_up = TRUE;
		if (c->index)
			voice_call_index(VOICE_hangup, c->index);
		else
			call_serial(VOICE, VOICE_hangupForegroundResumeBackground);
	}
	call_remove(c);
	call_audio_update();
	mm_gdbus_modem_voice_complete_delete_call(o, inv);
	return TRUE;
}

static gboolean on_voice_list(MmGdbusModemVoice *o, GDBusMethodInvocation *inv, gpointer u)
{
	GPtrArray *paths = g_ptr_array_new();
	guint i;
	for (i = 0; i < calls->len; i++)
		g_ptr_array_add(paths, ((Call *)calls->pdata[i])->path);
	g_ptr_array_add(paths, NULL);
	mm_gdbus_modem_voice_complete_list_calls(o, inv, (const gchar *const *)paths->pdata);
	g_ptr_array_free(paths, TRUE);
	return TRUE;
}

static gboolean on_voice_hangup_all(MmGdbusModemVoice *o, GDBusMethodInvocation *inv, gpointer u)
{
	guint i;
	for (i = 0; i < calls->len; i++) {
		Call *c = calls->pdata[i];
		if (!call_live(c))
			continue;
		c->hung_up = TRUE;
		if (c->index)
			voice_call_index(VOICE_hangup, c->index);
		else if (c->started)
			call_serial(VOICE, VOICE_hangupForegroundResumeBackground);
		else
			call_set_state(c, MM_CALL_STATE_TERMINATED, MM_CALL_STATE_REASON_TERMINATED);
	}
	clcc();
	mm_gdbus_modem_voice_complete_hangup_all(o, inv);
	return TRUE;
}

static gboolean on_voice_hold_accept(MmGdbusModemVoice *o, GDBusMethodInvocation *inv, gpointer u)
{
	if (vop_begin(inv, NULL, VOICER_switchWaitingOrHoldingAndActive))
		call_serial(VOICE, VOICE_switchWaitingOrHoldingAndActive);
	return TRUE;
}

static gboolean on_voice_hangup_accept(MmGdbusModemVoice *o, GDBusMethodInvocation *inv, gpointer u)
{
	if (vop_begin(inv, NULL, VOICER_hangupForegroundResumeBackground))
		call_serial(VOICE, VOICE_hangupForegroundResumeBackground);
	return TRUE;
}

static gboolean on_voice_transfer(MmGdbusModemVoice *o, GDBusMethodInvocation *inv, gpointer u)
{
	if (vop_begin(inv, NULL, VOICER_explicitCallTransfer))
		call_serial(VOICE, VOICE_explicitCallTransfer);
	return TRUE;
}

static void voice_on_response(guint code, int error, GBinderReader *r)
{
	if (code == VOICER_getCurrentCalls) {
		clcc_busy = FALSE;
		if (!error)
			read_call_list(r);
		if (clcc_again) {
			clcc_again = FALSE;
			clcc();
		}
	} else if (code == VOICER_sendDtmf) {
		if (error && dtmf_call && dtmf_call->dtmf)
			g_string_truncate(dtmf_call->dtmf, 0);  /* stop; dtmf_next completes the call */
		dtmf_next();
	} else if (code == VOICER_getLastCallFailCause && !error) {
		GBinderReader p;
		gboolean nn;
		gint32 cause = -1;
		if (gbinder_reader_start_parcelable(r, &p, &nn) && nn) {
			gbinder_reader_read_int32(&p, &cause);
			gbinder_reader_finish_parcelable(&p);
		}
		say("call: the network ended it, cause %d%s", cause, cause == 16 ? " (normal)" :
		    cause == 17 ? " (busy)" : cause == 21 ? " (rejected)" : cause == 1 ? " (no such number)" :
		    cause == 18 || cause == 19 ? " (no answer)" : "");
	} else if (vop_code && code == vop_code) {
		vop_done(error);
	} else if (code == VOICER_hangupConnection || code == VOICER_hangupForegroundResumeBackground) {
		clcc();                                     /* from DeleteCall / HangupAll */
	}
}

static void voice_on_indication(guint code, GBinderReader *r)
{
	if (code == VOICEI_callStateChanged || code == VOICEI_callRing || code == VOICEI_srvccStateNotify) {
		clcc();
	} else if (code == VOICEI_currentEmergencyNumberList) {
		gint32 n = 0, i;
		GPtrArray *a = g_ptr_array_new();
		gbinder_reader_read_int32(r, &n);
		for (i = 0; i < n && i < 64; i++) {
			GBinderReader p;
			gboolean nn;
			if (!gbinder_reader_start_parcelable(r, &p, &nn) || !nn)
				break;
			g_ptr_array_add(a, gbinder_reader_read_string16(&p));    /* EmergencyNumber.number */
			gbinder_reader_finish_parcelable(&p);
		}
		g_ptr_array_add(a, NULL);
		g_strfreev(ecc);
		ecc = (char **)g_ptr_array_free(a, FALSE);
		if (simobj && ecc[0])
			mm_gdbus_sim_set_emergency_numbers(simobj, (const gchar *const *)ecc);
	} else if (code == VOICEI_indicateRingbackTone) {
		gboolean on = FALSE;
		gbinder_reader_read_bool(r, &on);
		say("call: the network %s the ringing tone", on ? "wants us to play" : "plays");
	}
}

static void voice_rild_gone(void)
{
	guint i;
	if (vop_inv)
		g_dbus_method_invocation_return_error(vop_inv, MM_CORE_ERROR, MM_CORE_ERROR_ABORTED,
		                                      "rild went away");
	vop_inv = NULL;
	vop_call = NULL;
	vop_code = 0;
	clcc_busy = clcc_again = FALSE;
	for (i = 0; calls && i < calls->len; i++)
		call_set_state(calls->pdata[i], MM_CALL_STATE_TERMINATED, MM_CALL_STATE_REASON_ERROR);
}

static void voice_setup(void)
{
	calls = g_ptr_array_new();
	voice = mm_gdbus_modem_voice_skeleton_new();
	mm_gdbus_modem_voice_set_emergency_only(voice, FALSE);
	voice_list_changed();
	g_signal_connect(voice, "handle-create-call", G_CALLBACK(on_voice_create), NULL);
	g_signal_connect(voice, "handle-delete-call", G_CALLBACK(on_voice_delete), NULL);
	g_signal_connect(voice, "handle-list-calls", G_CALLBACK(on_voice_list), NULL);
	g_signal_connect(voice, "handle-hangup-all", G_CALLBACK(on_voice_hangup_all), NULL);
	g_signal_connect(voice, "handle-hold-and-accept", G_CALLBACK(on_voice_hold_accept), NULL);
	g_signal_connect(voice, "handle-hangup-and-accept", G_CALLBACK(on_voice_hangup_accept), NULL);
	g_signal_connect(voice, "handle-transfer", G_CALLBACK(on_voice_transfer), NULL);
	g_signal_connect(voice, "handle-call-waiting-setup", G_CALLBACK(on_unsupported), NULL);
	g_signal_connect(voice, "handle-call-waiting-query", G_CALLBACK(on_unsupported), NULL);
}

/* ---------- call audio (filled in below the D-Bus handlers) ----------------------------------- */

/* The voice itself never passes through Linux (the modem and the ABOX DSP exchange it directly);
 * the helper does what Samsung's audio HAL does around it: the DSP route, the call's virtual PCM
 * nodes, the modem's sound commands. One helper per call, one command per line on its stdin
 * ("device NAME", "mute 0|1", "start"); EOF = tear everything down and exit, so a bridge that
 * goes away mid-call leaves nothing behind. Its output goes to this log. */
#define CALLAUDIO_HELPER "/usr/local/sbin/s24-callaudio"

static gboolean call_audio_on;
static GPid ca_pid;
static gint ca_in = -1;
static gboolean ca_stopping;
static char ca_device[16] = "earpiece";
static gboolean ca_mute;

static void ca_send(const char *line)
{
	gsize n = strlen(line);
	if (ca_in < 0 || ca_stopping)
		return;
	if (write(ca_in, line, n) != (gssize)n || write(ca_in, "\n", 1) != 1)
		say("audio: the helper isn't listening (%s)", g_strerror(errno));
}

static void call_audio_start(void);

static void ca_exited(GPid pid, gint status, gpointer u)
{
	gboolean was_stopping = ca_stopping;
	g_spawn_close_pid(pid);
	if (pid != ca_pid)
		return;
	if (WIFEXITED(status))
		say("audio: helper done (exit %d)", WEXITSTATUS(status));
	else
		say("audio: helper killed by signal %d", WTERMSIG(status));
	ca_pid = 0;
	if (ca_in >= 0)
		close(ca_in);
	ca_in = -1;
	ca_stopping = FALSE;
	if (call_audio_on && was_stopping)  /* a new call came while the last one was torn down */
		call_audio_start();         /* (one that died by itself mid-call isn't restarted: no loop) */
}

static void call_audio_start(void)
{
	gchar *argv[] = { CALLAUDIO_HELPER, NULL };
	GError *err = NULL;
	char cmd[48];
	if (ca_pid)                         /* still tearing the last one down: ca_exited starts this */
		return;
	if (!g_file_test(CALLAUDIO_HELPER, G_FILE_TEST_IS_EXECUTABLE)) {
		say("audio: no helper (%s): calls have no sound", CALLAUDIO_HELPER);
		return;
	}
	if (!g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &ca_pid,
	                              &ca_in, NULL, NULL, &err)) {
		say("audio: can't start the helper: %s", err->message);
		g_error_free(err);
		ca_pid = 0;
		ca_in = -1;
		return;
	}
	g_child_watch_add(ca_pid, ca_exited, NULL);
	g_snprintf(cmd, sizeof cmd, "device %s", ca_device);
	ca_send(cmd);
	ca_send(ca_mute ? "mute 1" : "mute 0");
	ca_send("start");
}

static void call_audio_stop(void)
{
	if (!ca_pid || ca_stopping)
		return;
	ca_stopping = TRUE;
	close(ca_in);                       /* EOF: the helper tears down and exits */
	ca_in = -1;
}

static void call_audio_update(void)
{
	guint i;
	gboolean want = FALSE;
	for (i = 0; calls && i < calls->len; i++) {
		Call *c = calls->pdata[i];
		MMCallState s = mm_gdbus_call_get_state(c->obj);
		if (s == MM_CALL_STATE_DIALING || s == MM_CALL_STATE_RINGING_OUT || s == MM_CALL_STATE_ACTIVE ||
		    s == MM_CALL_STATE_HELD || (c->started && s == MM_CALL_STATE_UNKNOWN))
			want = TRUE;
	}
	if (want == call_audio_on)
		return;
	call_audio_on = want;
	say("audio: call audio %s", want ? "on" : "off");
	if (want)
		call_audio_start();
	else
		call_audio_stop();
}

/* ---------- D-Bus method handlers ------------------------------------------------------------ */

static gboolean on_enable(MmGdbusModem *o, GDBusMethodInvocation *inv, gboolean on, gpointer u)
{
	say("mm: Enable(%s)", on ? "true" : "false");
	if (inv_enable) {
		g_dbus_method_invocation_return_error(inv, MM_CORE_ERROR, MM_CORE_ERROR_IN_PROGRESS,
		                                      "already %s", en_target ? "enabling" : "disabling");
		return TRUE;
	}
	if (mm_state == MM_MODEM_STATE_FAILED || mm_state == MM_MODEM_STATE_LOCKED ||
	    mm_state == MM_MODEM_STATE_UNKNOWN || mm_state == MM_MODEM_STATE_INITIALIZING) {
		g_dbus_method_invocation_return_error(inv, MM_CORE_ERROR, MM_CORE_ERROR_WRONG_STATE,
		                                      "the modem isn't ready (%s)",
		                                      mm_modem_state_get_string(mm_state));
		return TRUE;
	}
	if ((on && st.radio == 10) || (!on && st.radio != 10 && st.cid < 0)) {
		mm_gdbus_modem_complete_enable(o, inv);
		return TRUE;
	}
	inv_enable = inv;
	start_enable(on);
	return TRUE;
}

static gboolean on_simple_connect(MmGdbusModemSimple *o, GDBusMethodInvocation *inv, GVariant *props,
                                  gpointer u)
{
	return start_connect(inv, TRUE, props);
}

static gboolean on_simple_disconnect(MmGdbusModemSimple *o, GDBusMethodInvocation *inv,
                                     const gchar *path, gpointer u)
{
	return start_disconnect(inv, TRUE);
}

static gboolean on_simple_status(MmGdbusModemSimple *o, GDBusMethodInvocation *inv, gpointer u)
{
	GVariantBuilder b;
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&b, "{sv}", "state", g_variant_new_int32(mm_state));
	g_variant_builder_add(&b, "{sv}", "signal-quality",
	                      mm_gdbus_modem_get_signal_quality(modem));
	g_variant_builder_add(&b, "{sv}", "access-technologies",
	                      g_variant_new_uint32(mm_gdbus_modem_get_access_technologies(modem)));
	g_variant_builder_add(&b, "{sv}", "m3gpp-registration-state",
	                      g_variant_new_uint32(mm_gdbus_modem3gpp_get_registration_state(m3gpp)));
	g_variant_builder_add(&b, "{sv}", "m3gpp-operator-code",
	                      g_variant_new_string(mm_gdbus_modem3gpp_get_operator_code(m3gpp)));
	g_variant_builder_add(&b, "{sv}", "m3gpp-operator-name",
	                      g_variant_new_string(mm_gdbus_modem3gpp_get_operator_name(m3gpp)));
	mm_gdbus_modem_simple_complete_get_status(o, inv, g_variant_builder_end(&b));
	return TRUE;
}

static gboolean on_create_bearer(MmGdbusModem *o, GDBusMethodInvocation *inv, GVariant *props,
                                 gpointer u)
{
	const char *apn = NULL;
	if (g_variant_lookup(props, "apn", "&s", &apn) && *apn) {
		g_free(co_apn);
		co_apn = g_strdup(apn);
	}
	update_bearer();
	mm_gdbus_modem_complete_create_bearer(o, inv, BEARER_PATH);
	return TRUE;
}

static gboolean on_list_bearers(MmGdbusModem *o, GDBusMethodInvocation *inv, gpointer u)
{
	const gchar *const list[] = { BEARER_PATH, NULL };
	const gchar *const none[] = { NULL };
	mm_gdbus_modem_complete_list_bearers(o, inv, bearer_exported ? list : none);
	return TRUE;
}

static gboolean on_delete_bearer(MmGdbusModem *o, GDBusMethodInvocation *inv, const gchar *path,
                                 gpointer u)
{
	mm_gdbus_modem_complete_delete_bearer(o, inv);   /* the one bearer stays */
	return TRUE;
}

static gboolean on_set_current_modes(MmGdbusModem *o, GDBusMethodInvocation *inv, GVariant *modes,
                                     gpointer u)
{
	guint allowed = 0, preferred = 0;
	int raf;
	g_variant_get(modes, "(uu)", &allowed, &preferred);
	raf = modes_to_raf(allowed == MM_MODEM_MODE_ANY ? 0xffffffff : allowed);
	if (!raf || inv_modes) {
		g_dbus_method_invocation_return_error(inv, MM_CORE_ERROR, MM_CORE_ERROR_INVALID_ARGS,
		                                      "unsupported modes");
		return TRUE;
	}
	say("mm: SetCurrentModes allowed 0x%x", allowed);
	inv_modes = inv;
	pending_raf = raf;
	set_allowed_types(raf);
	return TRUE;
}

static gboolean on_set_power_state(MmGdbusModem *o, GDBusMethodInvocation *inv, guint s, gpointer u)
{
	mm_gdbus_modem_complete_set_power_state(o, inv);
	return TRUE;
}

static gboolean on_unsupported(GDBusInterfaceSkeleton *o, GDBusMethodInvocation *inv, gpointer u)
{
	g_dbus_method_invocation_return_error(inv, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
	                                      "not supported on this modem");
	return TRUE;
}

static gboolean on_register(MmGdbusModem3gpp *o, GDBusMethodInvocation *inv, const gchar *op, gpointer u)
{
	if (op && *op) {
		g_dbus_method_invocation_return_error(inv, MM_CORE_ERROR, MM_CORE_ERROR_UNSUPPORTED,
		                                      "manual network selection isn't supported yet");
		return TRUE;
	}
	call_serial(NET, NET_setNetworkSelectionModeAutomatic);
	mm_gdbus_modem3gpp_complete_register(o, inv);
	return TRUE;
}

static gboolean on_bearer_connect(MmGdbusBearer *o, GDBusMethodInvocation *inv, gpointer u)
{
	return start_connect(inv, FALSE, NULL);
}

static gboolean on_bearer_disconnect(MmGdbusBearer *o, GDBusMethodInvocation *inv, gpointer u)
{
	return start_disconnect(inv, FALSE);
}

static gboolean on_mgr_scan(MmGdbusOrgFreedesktopModemManager1 *o, GDBusMethodInvocation *inv, gpointer u)
{
	mm_gdbus_org_freedesktop_modem_manager1_complete_scan_devices(o, inv);
	return TRUE;
}

static gboolean on_mgr_logging(MmGdbusOrgFreedesktopModemManager1 *o, GDBusMethodInvocation *inv,
                               const gchar *level, gpointer u)
{
	mm_gdbus_org_freedesktop_modem_manager1_complete_set_logging(o, inv);
	return TRUE;
}

/* ---------- RIL responses and indications ---------------------------------------------------- */

static void read_card_status(GBinderReader *r)
{
	GBinderReader p, a;
	gboolean nn;
	gint32 card = -1, upin, gsm_idx = -1, cdma_idx, ims_idx, napps = 0, i;
	if (!gbinder_reader_start_parcelable(r, &p, &nn) || !nn)
		return;
	gbinder_reader_read_int32(&p, &card);
	gbinder_reader_read_int32(&p, &upin);
	gbinder_reader_read_int32(&p, &gsm_idx);
	gbinder_reader_read_int32(&p, &cdma_idx);
	gbinder_reader_read_int32(&p, &ims_idx);
	gbinder_reader_read_int32(&p, &napps);
	st.card = card;
	st.app_state = -1;
	for (i = 0; i < napps && i < 16; i++) {
		gint32 type = 0, state = 0, perso = 0, pin1r, pin1 = 0, pin2;
		char *aid;
		if (!gbinder_reader_start_parcelable(&p, &a, &nn) || !nn)
			break;
		gbinder_reader_read_int32(&a, &type);
		gbinder_reader_read_int32(&a, &state);
		gbinder_reader_read_int32(&a, &perso);
		aid = gbinder_reader_read_string16(&a);
		gbinder_reader_skip_string16(&a);       /* label */
		gbinder_reader_read_int32(&a, &pin1r);
		gbinder_reader_read_int32(&a, &pin1);
		gbinder_reader_read_int32(&a, &pin2);
		gbinder_reader_finish_parcelable(&a);
		if (i == gsm_idx || (gsm_idx < 0 && i == 0)) {
			st.app_state = state;
			st.pin1 = pin1;
			g_free(st.aid);
			st.aid = aid;
		} else {
			g_free(aid);
		}
	}
	if (i == napps) {
		g_free(st.iccid);
		gbinder_reader_skip_string16(&p);       /* atr */
		st.iccid = gbinder_reader_read_string16(&p);
	}
	gbinder_reader_finish_parcelable(&p);
	say("sim: card %s, application %s", card == 0 ? "absent" : card == 1 ? "present" :
	    card == 2 ? "error" : "?", st.app_state == 5 ? "ready" : st.app_state == 2 ? "PIN" :
	    st.app_state == 3 ? "PUK" : "not ready");
}

static void read_reg_state(GBinderReader *r, int *reg, int *rat)
{
	GBinderReader p;
	gboolean nn;
	gint32 g = -1, t = -1, denial = 0;
	char *plmn = NULL;
	if (!gbinder_reader_start_parcelable(r, &p, &nn) || !nn)
		return;
	gbinder_reader_read_int32(&p, &g);
	gbinder_reader_read_int32(&p, &t);
	gbinder_reader_read_int32(&p, &denial);
	skip_union(&p);                                 /* cellIdentity */
	gbinder_reader_read_nullable_string16(&p, &plmn);
	gbinder_reader_finish_parcelable(&p);
	*reg = g;
	*rat = t;
	g_free(plmn);
}

static void read_signal(GBinderReader *r)
{
	GBinderReader p, q;
	gboolean nn;
	gint32 v[7], i;
	if (!gbinder_reader_start_parcelable(r, &p, &nn) || !nn)
		return;
	if (gbinder_reader_start_parcelable(&p, &q, &nn) && nn) {  /* gsm */
		gbinder_reader_read_int32(&q, &v[0]);
		gbinder_reader_finish_parcelable(&q);
		st.gsm_asu = v[0] == 99 ? UNSET : v[0];
	}
	gbinder_reader_skip_parcelable(&p);            /* cdma */
	gbinder_reader_skip_parcelable(&p);            /* evdo */
	if (gbinder_reader_start_parcelable(&p, &q, &nn) && nn) {  /* lte */
		for (i = 0; i < 7; i++)
			gbinder_reader_read_int32(&q, &v[i]);
		gbinder_reader_finish_parcelable(&q);
		st.lte_rsrp = v[1];
	}
	gbinder_reader_skip_parcelable(&p);            /* tdscdma */
	gbinder_reader_skip_parcelable(&p);            /* wcdma */
	if (gbinder_reader_start_parcelable(&p, &q, &nn) && nn) {  /* nr */
		gbinder_reader_read_int32(&q, &v[0]);
		gbinder_reader_finish_parcelable(&q);
		st.nr_rsrp = v[0];
	}
	gbinder_reader_finish_parcelable(&p);
}

/* SetupDataCallResult; returns its cid (or -1); keeps the call's settings if keep */
static int read_dc_result(GBinderReader *r, gboolean keep)
{
	GBinderReader p, a;
	gboolean nn;
	gint32 cause = 0, cid = -1, active = 0, type = 0, n = 0, i, mtu4 = 0, mtu6 = 0;
	gint64 retry = 0;
	char *ifname, **addrs, **dnses, **gws, **pcscf;
	if (!gbinder_reader_start_parcelable(r, &p, &nn) || !nn)
		return -1;
	gbinder_reader_read_int32(&p, &cause);
	gbinder_reader_read_int64(&p, &retry);
	gbinder_reader_read_int32(&p, &cid);
	gbinder_reader_read_int32(&p, &active);
	gbinder_reader_read_int32(&p, &type);
	ifname = gbinder_reader_read_string16(&p);
	gbinder_reader_read_int32(&p, &n);
	if (n < 0 || n > 16)
		n = 0;
	addrs = g_new0(char *, n + 1);
	for (i = 0; i < n; i++) {
		gint32 props;
		gint64 t1, t2;
		if (!gbinder_reader_start_parcelable(&p, &a, &nn) || !nn)
			break;
		addrs[i] = gbinder_reader_read_string16(&a);
		gbinder_reader_read_int32(&a, &props);
		gbinder_reader_read_int64(&a, &t1);
		gbinder_reader_read_int64(&a, &t2);
		gbinder_reader_finish_parcelable(&a);
	}
	dnses = read_str_array(&p);
	gws = read_str_array(&p);
	pcscf = read_str_array(&p);
	gbinder_reader_read_int32(&p, &mtu4);
	gbinder_reader_read_int32(&p, &mtu6);
	gbinder_reader_finish_parcelable(&p);
	if (keep && cause == 0 && active && ifname && *ifname && n > 0) {
		st.cid = cid;
		g_strlcpy(st.ifname, ifname, sizeof st.ifname);
		g_strfreev(st.addrs); g_strfreev(st.dnses); g_strfreev(st.gws);
		st.addrs = addrs; st.dnses = dnses; st.gws = gws;
		addrs = dnses = gws = NULL;
		st.mtu = mtu4 > 0 ? mtu4 : mtu6;
		say("data: call %d up on %s (%d address(es), mtu %d)", cid, st.ifname, n, st.mtu);
	} else if (keep) {
		say("data: call setup failed, cause %d, active %d", cause, active);
		cid = -1;
	}
	g_free(ifname);
	g_strfreev(addrs); g_strfreev(dnses); g_strfreev(gws); g_strfreev(pcscf);
	return cause == 0 ? cid : -1;
}

static void on_response(int s, guint code, GBinderReader *r)
{
	int type, error;
	if (code == SVC[s].ack_req)                     /* acknowledgeRequest(serial) */
		return;
	if (!read_info(r, &type, &error))
		return;
	if (type == 2)
		ack(s);
	switch (s) {
	case SIM:
		if (code == SIMR_getIccCardStatus && !error) {
			read_card_status(r);
			if (st.card == 1 && st.app_state == 5 && st.aid && !st.imsi)
				get_imsi();
			export_modem();                 /* rild really answers: the modem is usable */
			update_sim();
			update_state();
		} else if (code == SIMR_getImsiForApp) {
			char *imsi = error ? NULL : gbinder_reader_read_string16(r);
			if (imsi && strlen(imsi) >= 6) {
				g_free(st.imsi);
				st.imsi = imsi;
				g_strlcpy(st.hplmn, imsi, 3 + mnc_digits(imsi) + 1);
				say("sim: home network %s", st.hplmn);
				publish_sim_operator();
				update_sim();
			} else {
				g_free(imsi);
			}
		}
		break;
	case MODEM:
		if (code == MODEMR_getBasebandVersion && !error) {
			g_free(st.baseband);
			st.baseband = gbinder_reader_read_string16(r);
			update_modem_props();
		} else if (code == MODEMR_getImei && !error) {
			GBinderReader p;
			gboolean nn;
			gint32 t;
			if (gbinder_reader_start_parcelable(r, &p, &nn) && nn) {
				gbinder_reader_read_int32(&p, &t);
				g_free(st.imei);
				st.imei = gbinder_reader_read_string16(&p);
				gbinder_reader_finish_parcelable(&p);
				update_modem_props();
			}
		} else if (code == MODEMR_setRadioPower) {
			say("ril: radio power request: %s", radio_err(error));
			if (en_step == EN_POWER) {
				if (!error)
					st.radio = en_target ? 10 : 0;
				finish_enable(!error, radio_err(error));
			}
		}
		break;
	case NET:
		if (code == NETR_getDataRegistrationState) {
			/* the radio state at start: rild answers this even with the radio off ("not
			 * registered, not searching"); searching or registered means it's on */
			if (!error) {
				read_reg_state(r, &st.data_reg, &st.data_rat);
				if (st.radio < 0)
					st.radio = st.data_reg == 0 || st.data_reg == 4 ? 0 : 10;
			} else if (error == 1 && st.radio < 0) {  /* RADIO_NOT_AVAILABLE */
				st.radio = 0;
			}
			update_registration();
			update_state();
			connect_advance();
		} else if (code == NETR_getVoiceRegistrationState && !error) {
			read_reg_state(r, &st.voice_reg, &st.voice_rat);
			update_registration();
			update_state();
		} else if (code == NETR_getOperator && !error) {
			char *l = gbinder_reader_read_string16(r);
			char *sh = gbinder_reader_read_string16(r);
			char *num = gbinder_reader_read_string16(r);
			g_strlcpy(st.op_long, l ? l : "", sizeof st.op_long);
			g_strlcpy(st.op_short, sh ? sh : "", sizeof st.op_short);
			g_strlcpy(st.op_numeric, num ? num : "", sizeof st.op_numeric);
			g_free(l); g_free(sh); g_free(num);
			update_registration();
			update_sim();
		} else if (code == NETR_getSignalStrength && !error) {
			read_signal(r);
			update_registration();
		} else if (code == NETR_getAllowedNetworkTypesBitmap && !error) {
			gbinder_reader_read_int32(r, &st.allowed);
			update_modem_props();
		} else if (code == NETR_setAllowedNetworkTypesBitmap) {
			if (inv_modes) {
				if (!error) {
					st.allowed = pending_raf;
					update_modem_props();
					mm_gdbus_modem_complete_set_current_modes(modem, inv_modes);
				} else {
					g_dbus_method_invocation_return_error(inv_modes, MM_CORE_ERROR,
					                                      MM_CORE_ERROR_FAILED, "%s",
					                                      radio_err(error));
				}
				inv_modes = NULL;
			}
		}
		break;
	case DATA:
		if (code == DATAR_setInitialAttachApn) {
			if (en_step == EN_ATTACH_APN)
				enable_advance();
			else if (co_step == CO_ATTACH_APN) {
				co_step = CO_SETUP;
				setup_data_call(co_apn);
			}
		} else if (code == DATAR_setDataAllowed) {
			if (en_step == EN_ALLOW)
				enable_advance();
		} else if (code == DATAR_setupDataCall && co_step == CO_SETUP) {
			if (error || read_dc_result(r, TRUE) < 0 || st.cid < 0) {
				fail_connect(error ? radio_err(error) : "the network refused the call");
			} else {
				update_bearer();
				run_hook("up");
				finish_connect();
			}
		} else if (code == DATAR_deactivateDataCall) {
			say("data: call released (%s)", radio_err(error));
			if (inv_disconnect)
				finish_disconnect();
			else if (st.cid >= 0 && en_step == EN_POWER)
				bearer_gone("radio off");
		}
		break;
	case MSG:
		if ((code == MSGR_sendSms || code == MSGR_sendSmsExpectMore) && sending) {
			GBinderReader p;
			gboolean nn;
			gint32 mref = 0, ecode = 0;
			if (gbinder_reader_start_parcelable(r, &p, &nn) && nn) {   /* SendSmsResult */
				gbinder_reader_read_int32(&p, &mref);
				gbinder_reader_skip_string16(&p);   /* ackPDU */
				gbinder_reader_read_int32(&p, &ecode);
				gbinder_reader_finish_parcelable(&p);
			}
			if (error) {
				char *why = g_strdup_printf("%s, network error %d", radio_err(error), ecode);
				sms_send_done(why);
				g_free(why);
			} else if (++sending->next < sending->pdus->len) {
				send_next_part();
			} else {
				mm_gdbus_sms_set_message_reference(sending->obj, (guint)mref);
				sms_send_done(NULL);
			}
		}
		break;
	case VOICE:
		voice_on_response(code, error, r);
		break;
	}
}

static void on_indication(int s, guint code, GBinderReader *r)
{
	gint32 type = 0, v = 0;
	gbinder_reader_read_int32(r, &type);
	if (type == 1)
		ack(s);
	switch (s) {
	case MODEM:
		if (code == MODEMI_radioStateChanged) {
			gbinder_reader_read_int32(r, &v);
			st.radio = v;
			say("ril: radio %s", v == 10 ? "on" : v == 0 ? "off" : "unavailable");
			if (v == 10)
				query_registration();
			else if (st.cid >= 0)
				bearer_gone("radio off");
			update_state();
			connect_advance();
		}
		break;
	case SIM:
		if (code == SIMI_simStatusChanged)
			call_serial(SIM, SIM_getIccCardStatus);
		break;
	case NET:
		if (code == NETI_networkStateChanged) {
			query_registration();
		} else if (code == NETI_currentSignalStrength) {
			read_signal(r);
			update_registration();
		}
		break;
	case DATA:
		if (code == DATAI_dataCallListChanged && st.cid >= 0 && !inv_disconnect) {
			gint32 n = 0, i, found = 0;
			gbinder_reader_read_int32(r, &n);
			for (i = 0; i < n && i < 8; i++)
				if (read_dc_result(r, FALSE) == st.cid)
					found = 1;
			if (!found)
				bearer_gone("dropped by the network");
		}
		break;
	case MSG:
		if (code == MSGI_newSms) {
			gsize len = 0;
			const guint8 *pdu = gbinder_reader_read_byte_array(r, &len);
			SmsPart *p = pdu && len ? decode_deliver(pdu, len) : NULL;
			ack_incoming_sms();                     /* or the network sends it again */
			if (p)
				sms_part_in(p);
			else
				say("sms: an incoming PDU (%u bytes) couldn't be read", (guint)len);
		} else if (code == MSGI_newSmsStatusReport) {
			ack_incoming_sms();
			say("sms: delivery report");
		}
		break;
	case VOICE:
		voice_on_indication(code, r);
		break;
	}
}

static GBinderLocalReply *on_transact(GBinderLocalObject *obj, GBinderRemoteRequest *req,
                                      guint code, guint flags, int *status, void *user)
{
	const CbKind *k = user;
	GBinderReader r;
	*status = 0;
	if (code == AIDL_GET_VERSION || code == AIDL_GET_HASH) {
		GBinderLocalReply *rep = gbinder_local_object_new_reply(obj);
		GBinderWriter w;
		gbinder_local_reply_init_writer(rep, &w);
		gbinder_writer_append_int32(&w, 0);     /* EX_NONE */
		if (code == AIDL_GET_VERSION)
			gbinder_writer_append_int32(&w, 2);
		else
			gbinder_writer_append_string16(&w, SVC[k->svc].hash);
		return rep;
	}
	gbinder_remote_request_init_reader(req, &r);
	if (k->indication)
		on_indication(k->svc, code, &r);
	else
		on_response(k->svc, code, &r);
	return NULL;
}

/* ---------- the modem on D-Bus ------------------------------------------------------------------
 * Published once rild has really answered (the SIM status), removed while rild is gone: a modem
 * that drops to "disabled" makes NetworkManager think somebody switched it off (no autoconnect);
 * one that disappears and comes back is a new device it simply connects again, as when
 * ModemManager's own modems reset. */

static void export_modem(void)
{
	if (modem_exported)
		return;
	modem_exported = TRUE;
	update_modem_props();
	update_sim();
	g_dbus_object_manager_server_export(om, G_DBUS_OBJECT_SKELETON(mobj));
	say("mm: modem published at %s", MODEM_PATH);
}

static void unexport_modem(void)
{
	const gchar *const none[] = { NULL };
	if (!modem_exported)
		return;
	g_dbus_object_manager_server_unexport(om, MODEM_PATH);
	if (sim_exported)
		g_dbus_interface_skeleton_unexport(G_DBUS_INTERFACE_SKELETON(simobj));
	if (bearer_exported)
		g_dbus_interface_skeleton_unexport(G_DBUS_INTERFACE_SKELETON(bearer));
	sim_exported = bearer_exported = modem_exported = FALSE;
	mm_gdbus_modem_set_sim(modem, "/");
	mm_gdbus_modem_set_bearers(modem, none);
	mm_state = MM_MODEM_STATE_UNKNOWN;
	mm_gdbus_modem_set_state(modem, MM_MODEM_STATE_UNKNOWN);
	say("mm: modem removed while rild is gone");
}

/* A rild that was restarted while the modem processor kept running answers nothing useful
 * ("Invalid response: NULL") until the modem processor reboots (cbd), as seen 2026-11-15. */
static guint health_timer;
static gint64 last_cp_reboot;

static gboolean health_check(gpointer unused)
{
	static int tries;
	if (!st.up || st.card >= 0) {
		health_timer = 0;
		tries = 0;
		return G_SOURCE_REMOVE;
	}
	if (++tries < 6) {                              /* ask again every 5 s for 30 s */
		call_serial(SIM, SIM_getIccCardStatus);
		return G_SOURCE_CONTINUE;
	}
	tries = 0;
	if (g_get_monotonic_time() - last_cp_reboot > 120 * G_USEC_PER_SEC) {
		gchar *args[] = { "chroot", "/android", "/system/bin/setprop", "ctl.restart",
		                  "cpboot-daemon", NULL };
		say("ril: rild hasn't read the SIM for 30 s: rebooting the modem processor");
		last_cp_reboot = g_get_monotonic_time();
		g_spawn_sync(NULL, args, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
		             G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, NULL, NULL);
	}
	return G_SOURCE_CONTINUE;
}

/* ---------- connecting to rild --------------------------------------------------------------- */

static gboolean ril_connect(gpointer unused);

static void ril_down(void)
{
	int s, i;
	say("ril: lost rild, reconnecting");
	for (s = 0; s < NSVC; s++) {
		if (remote[s] && death_id[s])
			gbinder_remote_object_remove_handler(remote[s], death_id[s]);
		death_id[s] = 0;
		if (client[s])
			gbinder_client_unref(client[s]);
		if (remote[s])
			gbinder_remote_object_unref(remote[s]);
		client[s] = NULL;
		remote[s] = NULL;
		for (i = 0; i < 2; i++) {
			if (cb_obj[s][i])
				gbinder_local_object_unref(cb_obj[s][i]);
			cb_obj[s][i] = NULL;
		}
	}
	st.up = FALSE;
	st.radio = -1;
	if (st.cid >= 0)
		bearer_gone("rild went away");
	if (inv_connect)
		fail_connect("rild went away");
	if (inv_enable)
		finish_enable(FALSE, "rild went away");
	if (sending)
		sms_send_done("rild went away");
	voice_rild_gone();
	unexport_modem();
	/* a new rild knows nothing of what this one was told */
	st.card = st.app_state = st.data_reg = st.voice_reg = -1;
	st.lte_rsrp = st.nr_rsrp = st.gsm_asu = UNSET;
	g_free(st.ia_apn);
	st.ia_apn = NULL;
	g_timeout_add_seconds(3, ril_connect, NULL);
}

static void on_death(GBinderRemoteObject *obj, void *user)
{
	if (st.up)
		ril_down();
}

static gboolean ril_connect(gpointer unused)
{
	const char *slot = getenv("S24_SLOT");
	int s, i;
	if (!sm)
		sm = gbinder_servicemanager_new("/dev/binder");
	if (!sm)
		return G_SOURCE_CONTINUE;
	for (s = 0; s < NSVC; s++) {
		char *fq = g_strdup_printf("%s/slot%s", SVC[s].iface, slot ? slot : "1");
		int status = 0;
		GBinderRemoteObject *o = gbinder_servicemanager_get_service_sync(sm, fq, &status);
		g_free(fq);
		if (!o && (s == MSG || s == VOICE)) {       /* SMS and calls are optional */
			say("ril: no %s service: no %s", SVC[s].name, s == MSG ? "SMS" : "calls");
			continue;
		}
		if (!o) {
			static int told;
			if (!told++)
				say("ril: waiting for rild");
			for (i = 0; i < s; i++) {
				gbinder_client_unref(client[i]);
				gbinder_remote_object_unref(remote[i]);
				client[i] = NULL;
				remote[i] = NULL;
			}
			return G_SOURCE_CONTINUE;
		}
		remote[s] = gbinder_remote_object_ref(o);
		client[s] = gbinder_client_new(remote[s], SVC[s].iface);
	}
	for (s = 0; s < NSVC; s++) {
		GBinderWriter w;
		GBinderLocalRequest *r;
		if (!client[s])
			continue;
		for (i = 0; i < 2; i++) {
			char *iface = g_strconcat(SVC[s].iface, i ? "Indication" : "Response", NULL);
			cb_kind[s][i].svc = s;
			cb_kind[s][i].indication = i;
			cb_obj[s][i] = gbinder_servicemanager_new_local_object(sm, iface, on_transact,
			                                                       &cb_kind[s][i]);
			gbinder_local_object_set_stability(cb_obj[s][i], GBINDER_STABILITY_VINTF);
			g_free(iface);
		}
		r = req_new(s, &w);
		gbinder_writer_append_local_object(&w, cb_obj[s][0]);
		gbinder_writer_append_local_object(&w, cb_obj[s][1]);
		send_req(s, SVC[s].set_resp, r);
		death_id[s] = gbinder_remote_object_add_death_handler(remote[s], on_death, NULL);
	}
	st.up = TRUE;
	st.card = -1;
	say("ril: connected to rild");
	if (!health_timer)
		health_timer = g_timeout_add_seconds(5, health_check, NULL);
	call_serial(SIM, SIM_getIccCardStatus);
	call_serial(MODEM, MODEM_getBasebandVersion);
	call_serial(MODEM, MODEM_getImei);
	call_serial(NET, NET_getAllowedNetworkTypesBitmap);
	if (client[MSG]) {                              /* room for SMS: the network may deliver */
		GBinderWriter w;
		GBinderLocalRequest *r = req_new(MSG, &w);
		gbinder_writer_append_int32(&w, next_serial++);
		gbinder_writer_append_bool(&w, TRUE);
		send_req(MSG, MSG_reportSmsMemoryStatus, r);
	}
	query_registration();
	clcc();                                         /* calls that are already there */
	update_state();
	return G_SOURCE_REMOVE;
}

/* ---------- D-Bus setup ---------------------------------------------------------------------- */

/* rild answers only the last program that registered with it: talk to it only once this process
 * owns the name, so a second copy that loses the name never takes the answers away */
static void on_name_acquired(GDBusConnection *c, const gchar *name, gpointer u)
{
	say("mm: owns %s", name);
	if (!st.up && ril_connect(NULL) == G_SOURCE_CONTINUE)
		g_timeout_add_seconds(2, ril_connect, NULL);
}

static void on_name_lost(GDBusConnection *c, const gchar *name, gpointer u)
{
	say("mm: can't own %s (another s24-mmd or ModemManager has it): exiting", name);
	exit(1);
}

static void export_all(void)
{
	GError *err = NULL;
	mgr = mm_gdbus_org_freedesktop_modem_manager1_skeleton_new();
	mm_gdbus_org_freedesktop_modem_manager1_set_version(mgr, "1.24.0-s24");
	g_signal_connect(mgr, "handle-scan-devices", G_CALLBACK(on_mgr_scan), NULL);
	g_signal_connect(mgr, "handle-set-logging", G_CALLBACK(on_mgr_logging), NULL);
	if (!g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(mgr), bus, MM_PATH, &err)) {
		say("mm: manager export failed: %s", err->message);
		g_clear_error(&err);
	}
	om = g_dbus_object_manager_server_new(MM_PATH);

	modem = mm_gdbus_modem_skeleton_new();
	m3gpp = mm_gdbus_modem3gpp_skeleton_new();
	simple = mm_gdbus_modem_simple_skeleton_new();
	simobj = mm_gdbus_sim_skeleton_new();
	bearer = mm_gdbus_bearer_skeleton_new();
	mm_gdbus_bearer_set_bearer_type(bearer, MM_BEARER_TYPE_DEFAULT);
	mm_gdbus_bearer_set_ip_timeout(bearer, 20);
	mm_gdbus_bearer_set_profile_id(bearer, -1);
	mm_gdbus_bearer_set_connection_error(bearer, g_variant_new("(ss)", "", ""));
	mm_gdbus_bearer_set_stats(bearer, g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0));
	mm_gdbus_bearer_set_ip4_config(bearer, g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0));
	mm_gdbus_bearer_set_ip6_config(bearer, g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0));
	mm_gdbus_bearer_set_properties(bearer, g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0));
	mm_gdbus_modem3gpp_set_initial_eps_bearer(m3gpp, "/");
	mm_gdbus_modem3gpp_set_initial_eps_bearer_settings(m3gpp,
		g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0));
	mm_gdbus_modem3gpp_set_pco(m3gpp, g_variant_new_array(G_VARIANT_TYPE("(ubay)"), NULL, 0));
	mm_gdbus_modem3gpp_set_nr5g_registration_settings(m3gpp,
		g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0));
	mm_gdbus_sim_set_preferred_networks(simobj, g_variant_new_array(G_VARIANT_TYPE("(su)"), NULL, 0));
	mm_gdbus_sim_set_gid1(simobj, g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, "", 0, 1));
	mm_gdbus_sim_set_gid2(simobj, g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, "", 0, 1));

	g_signal_connect(modem, "handle-enable", G_CALLBACK(on_enable), NULL);
	g_signal_connect(modem, "handle-create-bearer", G_CALLBACK(on_create_bearer), NULL);
	g_signal_connect(modem, "handle-list-bearers", G_CALLBACK(on_list_bearers), NULL);
	g_signal_connect(modem, "handle-delete-bearer", G_CALLBACK(on_delete_bearer), NULL);
	g_signal_connect(modem, "handle-set-current-modes", G_CALLBACK(on_set_current_modes), NULL);
	g_signal_connect(modem, "handle-set-power-state", G_CALLBACK(on_set_power_state), NULL);
	g_signal_connect(modem, "handle-reset", G_CALLBACK(on_unsupported), NULL);
	g_signal_connect(modem, "handle-factory-reset", G_CALLBACK(on_unsupported), NULL);
	g_signal_connect(modem, "handle-command", G_CALLBACK(on_unsupported), NULL);
	g_signal_connect(modem, "handle-set-current-bands", G_CALLBACK(on_unsupported), NULL);
	g_signal_connect(modem, "handle-set-current-capabilities", G_CALLBACK(on_unsupported), NULL);
	g_signal_connect(modem, "handle-get-cell-info", G_CALLBACK(on_unsupported), NULL);
	g_signal_connect(m3gpp, "handle-register", G_CALLBACK(on_register), NULL);
	g_signal_connect(m3gpp, "handle-scan", G_CALLBACK(on_unsupported), NULL);
	g_signal_connect(simple, "handle-connect", G_CALLBACK(on_simple_connect), NULL);
	g_signal_connect(simple, "handle-disconnect", G_CALLBACK(on_simple_disconnect), NULL);
	g_signal_connect(simple, "handle-get-status", G_CALLBACK(on_simple_status), NULL);
	g_signal_connect(bearer, "handle-connect", G_CALLBACK(on_bearer_connect), NULL);
	g_signal_connect(bearer, "handle-disconnect", G_CALLBACK(on_bearer_disconnect), NULL);
	g_signal_connect(simobj, "handle-send-pin", G_CALLBACK(on_unsupported), NULL);
	g_signal_connect(simobj, "handle-send-puk", G_CALLBACK(on_unsupported), NULL);
	g_signal_connect(simobj, "handle-enable-pin", G_CALLBACK(on_unsupported), NULL);
	g_signal_connect(simobj, "handle-change-pin", G_CALLBACK(on_unsupported), NULL);

	mm_gdbus_modem_set_state(modem, MM_MODEM_STATE_UNKNOWN);
	mm_gdbus_modem_set_signal_quality(modem, g_variant_new("(ub)", 0u, FALSE));
	update_modem_props();
	mobj = mm_gdbus_object_skeleton_new(MODEM_PATH);
	mm_gdbus_object_skeleton_set_modem(mobj, modem);
	mm_gdbus_object_skeleton_set_modem3gpp(mobj, m3gpp);
	mm_gdbus_object_skeleton_set_modem_simple(mobj, simple);
	messaging_setup();
	mm_gdbus_object_skeleton_set_modem_messaging(mobj, messaging);
	voice_setup();
	mm_gdbus_object_skeleton_set_modem_voice(mobj, voice);
	g_dbus_object_manager_server_set_connection(om, bus);   /* the modem comes with rild's answers */
}

static gboolean on_signal(gpointer unused)
{
	say("mm: stopping");
	if (st.cid >= 0 && st.up) {
		run_hook("down");
		deactivate_data_call(st.cid);
	}
	g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
	GError *err = NULL;
	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGPIPE, SIG_IGN);       /* a call-audio helper that died must not take the bridge along */
	/* PDU tests, nothing sent: --decode HEX (SMSC + SMS-DELIVER) | --encode NUMBER TEXT */
	if (argc == 3 && !strcmp(argv[1], "--decode")) {
		gsize n = strlen(argv[2]) / 2, i;
		guint8 *b = g_new0(guint8, n + 1);
		SmsPart *p;
		for (i = 0; i < n; i++)
			sscanf(argv[2] + 2 * i, "%2hhx", &b[i]);
		p = decode_deliver(b, n);
		if (!p)
			return 1;
		printf("from %s smsc %s time %s class %d part %u/%u ref %u\ntext: %s\n", p->number,
		       p->smsc ? p->smsc : "-", p->timestamp, p->klass, p->seq, p->total, p->ref, p->text);
		return 0;
	}
	if (argc == 4 && !strcmp(argv[1], "--encode")) {
		GPtrArray *a = encode_submit(argv[2], argv[3]);
		guint i;
		for (i = 0; i < a->len; i++)
			printf("%s\n", (char *)a->pdata[i]);
		return 0;
	}
	loop = g_main_loop_new(NULL, FALSE);
	bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
	if (!bus) {
		fprintf(stderr, "s24-mmd: no system bus: %s\n", err->message);
		return 1;
	}
	export_all();
	g_bus_own_name_on_connection(bus, "org.freedesktop.ModemManager1", G_BUS_NAME_OWNER_FLAGS_NONE,
	                             on_name_acquired, on_name_lost, NULL, NULL);
	g_unix_signal_add(SIGINT, on_signal, NULL);
	g_unix_signal_add(SIGTERM, on_signal, NULL);
	g_main_loop_run(loop);   /* rild comes in on_name_acquired */
	return 0;
}
