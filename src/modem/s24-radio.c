/*
 * s24-radio: a small client for the Samsung RIL on the Galaxy S24 running postmarketOS
 * (Claude, 2026-11-14). rild runs in the Android modem container (android-ctr0.sh) and offers
 * Android's stable AIDL radio HAL, version 2 (android.hardware.radio.*). This program talks to
 * it straight over /dev/binder with libgbinder (no Android framework involved).
 *
 *   s24-radio status              SIM, radio, registration, operator and signal
 *   s24-radio power on|off        radio (airplane mode off/on)
 *   s24-radio data [APN|auto]     bring up mobile data and keep it up until stopped
 *                                 (SIGINT/SIGTERM tears the call down). In Android's order: SIM,
 *                                 IMSI, publish the SIM operator, attach APN, allow data, radio
 *                                 on, register (LTE preferred), call. No APN or "auto" = from the
 *                                 phone's own APN list. S24_RADIO_HOOK=prog is run as
 *                                 "prog up IFACE DNS..." / "prog down IFACE".
 *   s24-radio net [all|0xBITS]    show (or set) the network types the modem may use; "all" =
 *                                 2G + 3G + 4G + 5G, what Android's "auto" mode asks for. Without
 *                                 Android's phone service nobody sets them, so the modem may stay
 *                                 on whatever it had.
 *   s24-radio usage [voice|data]  show (or set) the modem's usage setting (3GPP TS 24.301): a
 *                                 voice-centric phone that can't make calls over LTE (no VoLTE:
 *                                 Samsung's IMS stack is Android framework code, not running here)
 *                                 turns LTE off and falls back to 2G; data-centric stays on LTE.
 * S24_SLOT=2 in the environment talks to the second SIM slot.
 *
 * Wire format notes (stable AIDL over libbinder): method codes are the declaration order in the
 * frozen aidl_api/<package>/2 files, starting at 1; every parcelable is an int32 non-null marker,
 * an int32 size (counting itself) and its fields; a union is a marker, an int32 tag and the value;
 * bool is an int32; String is UTF-16 with an int32 length (-1 = null).
 * Identifiers (IMSI, ICCID, IMEI) are never printed in full: only the MCC/MNC part of the IMSI.
 */
#include <gbinder.h>
#include <glib.h>
#include <glib-unix.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SIM, MODEM, NET, DATA, CFG, NSVC };

static const struct {
	const char *name, *iface, *instance, *hash;
	guint32 set_resp, resp_ack;  /* setResponseFunctions, responseAcknowledgement (0: none) */
} SVC[NSVC] = {
	{ "sim", "android.hardware.radio.sim.IRadioSim", "slot1",
	  "4f348cc7aca716cc41c09ea95895c4b261231035", 28, 20 },
	{ "modem", "android.hardware.radio.modem.IRadioModem", "slot1",
	  "09927560afccc75a063944fbbab3af48099261ca", 17, 13 },
	{ "network", "android.hardware.radio.network.IRadioNetwork", "slot1",
	  "1b6608f238bd0b1c642df315621a7b605eafc883", 28, 16 },
	{ "data", "android.hardware.radio.data.IRadioData", "slot1",
	  "cb458326b02e0e87143f24118543e8cc7d6a9e8e", 12, 7 },
	{ "config", "android.hardware.radio.config.IRadioConfig", "default",
	  "0be135cf3de9586d6aabb58cb6af0ba425431743", 7, 0 },
};

/* requests */
#define SIM_getIccCardStatus 9
#define SIM_getImsiForApp 10
#define MODEM_getBasebandVersion 2
#define MODEM_setRadioPower 16
#define NET_getAllowedNetworkTypesBitmap 1
#define NET_setAllowedNetworkTypesBitmap 17
#define NET_getDataRegistrationState 7
#define NET_getOperator 10
#define NET_getSignalStrength 11
#define NET_getVoiceRegistrationState 14
#define NET_setNetworkSelectionModeAutomatic 25
#define NET_getImsRegistrationState 8
#define NET_setUsageSetting 35
#define NET_getUsageSetting 36
#define DATA_deactivateDataCall 3
#define DATA_getDataCallList 4
#define DATA_setDataAllowed 8
#define DATA_setInitialAttachApn 11
#define DATA_setupDataCall 13
#define CFG_getSimSlotsStatus 4
/* responses */
#define SIMR_getIccCardStatus 10
#define SIMR_getImsiForApp 11
#define MODEMR_getBasebandVersion 3
#define MODEMR_setRadioPower 16
#define NETR_getAllowedNetworkTypesBitmap 2
#define NETR_setAllowedNetworkTypesBitmap 17
#define NETR_getDataRegistrationState 8
#define NETR_getOperator 11
#define NETR_getSignalStrength 12
#define NETR_getVoiceRegistrationState 15
#define NETR_setNetworkSelectionModeAutomatic 27
#define NETR_getImsRegistrationState 9
#define NETR_setUsageSetting 34
#define NETR_getUsageSetting 35
#define DATAR_deactivateDataCall 4
#define DATAR_getDataCallList 5
#define DATAR_setDataAllowed 8
#define DATAR_setInitialAttachApn 11
#define DATAR_setupDataCall 12
#define CFGR_getSimSlotsStatus 4
/* indications */
#define MODEMI_radioStateChanged 4
#define MODEMI_rilConnected 5
#define SIMI_simStatusChanged 6
#define NETI_currentSignalStrength 6
#define NETI_networkStateChanged 9
#define DATAI_dataCallListChanged 1

#define AIDL_GET_VERSION 0x00ffffff
#define AIDL_GET_HASH 0x00fffffe

typedef struct { int svc; gboolean indication; } CbKind;

static GMainLoop *loop;
static GBinderServiceManager *sm;
static GBinderClient *client[NSVC];
static GBinderRemoteObject *remote[NSVC];
static GBinderLocalObject *cb_obj[NSVC][2];
static CbKind cb_kind[NSVC][2];
static int next_serial = 1;
static int exit_code = 0;

/* what we know */
static struct {
	int radio;                      /* RadioState: 0 off, 1 unavailable, 10 on; -1 unknown */
	int card, app_type, app_state;  /* -1 unknown */
	int data_reg, data_rat, voice_reg, voice_rat;
	char plmn[16], op_long[64], op_numeric[16], imsi_plmn[8];
	int lte_rsrp, lte_rsrq, nr_rsrp;
	int cid;                        /* active data call, -1 none */
	char ifname[32];
	char *aid;                      /* the SIM's USIM/SIM application (for getImsiForApp) */
} st = { -1, -1, -1, -1, -1, -1, -1, -1, "", "", "", "", 0x7fffffff, 0x7fffffff, 0x7fffffff, -1, "",
         NULL };

static const char *mode;            /* "status", "power", "data", "net" */
/* RadioAccessFamily: GSM GPRS EDGE | UMTS HSDPA HSUPA HSPA HSPA+ | LTE LTE-CA | NR */
#define RAF_ALL (65536 | 2 | 4 | 8 | 512 | 1024 | 2048 | 32768 | 16384 | 524288 | 1048576)
static int want_types;              /* net: bitmap to set, 0 = only show */
static int want_usage;              /* usage: 1 voice-centric, 2 data-centric, 0 = only show */
static const char *apn = "";
static int want_power = 1;
static int step;                    /* data: state machine step */
static guint reg_timer;
static int reg_tries;

/* ---------------------------------------------------------------------------------------- */

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
	case 39: return "SYSTEM_ERR";
	case 40: return "MODEM_ERR";
	case 41: return "INVALID_STATE";
	case 44: return "INVALID_ARGUMENTS";
	case 45: return "INVALID_SIM_STATE";
	case 46: return "INVALID_MODEM_STATE";
	case 61: return "NOT_PROVISIONED";
	case 62: return "NO_SUBSCRIPTION";
	case 63: return "NO_NETWORK_FOUND";
	}
	return "error";
}

static const char *reg_name(int r)
{
	switch (r) {
	case 0: return "not registered, not searching";
	case 1: return "registered (home)";
	case 2: return "searching";
	case 3: return "registration denied";
	case 4: return "unknown";
	case 5: return "registered (roaming)";
	case 10: case 12: case 13: case 14: case 20: return "emergency calls only";
	}
	return "?";
}

static const char *rat_name(int r)
{
	static const char *n[] = { "unknown", "GPRS", "EDGE", "UMTS", "IS95A", "IS95B", "1xRTT",
		"EVDO_0", "EVDO_A", "HSDPA", "HSUPA", "HSPA", "EVDO_B", "EHRPD", "LTE", "HSPA+", "GSM",
		"TD_SCDMA", "IWLAN", "LTE_CA", "NR" };
	return r >= 0 && r < (int)G_N_ELEMENTS(n) ? n[r] : "?";
}

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

static gboolean registered(int r) { return r == 1 || r == 5; }

/* ---------------------------------------------------------------------------------------- */

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
		fprintf(stderr, "s24-radio: %s call %u failed: %d\n", SVC[s].name, code, status);
	return status;
}

static int call_serial(int s, guint32 code)
{
	GBinderWriter w;
	GBinderLocalRequest *r = req_new(s, &w);
	int serial = next_serial++;
	gbinder_writer_append_int32(&w, serial);
	send_req(s, code, r);
	return serial;
}

static void ack(int s)
{
	GBinderWriter w;
	if (SVC[s].resp_ack)
		send_req(s, SVC[s].resp_ack, req_new(s, &w));
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

/* DataProfileInfo for the default internet APN */
static void write_profile(GBinderWriter *w)
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

static void set_initial_attach_apn(void)
{
	GBinderWriter w;
	GBinderLocalRequest *r = req_new(DATA, &w);
	gbinder_writer_append_int32(&w, next_serial++);
	write_profile(&w);
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

static void setup_data_call(void)
{
	GBinderWriter w;
	GBinderLocalRequest *r = req_new(DATA, &w);
	gbinder_writer_append_int32(&w, next_serial++);
	gbinder_writer_append_int32(&w, access_network(st.data_rat));
	write_profile(&w);
	gbinder_writer_append_bool(&w, TRUE);           /* roamingAllowed */
	gbinder_writer_append_int32(&w, 1);             /* reason: NORMAL */
	gbinder_writer_append_int32(&w, 0);             /* addresses: none */
	gbinder_writer_append_int32(&w, 0);             /* dnses: none */
	gbinder_writer_append_int32(&w, 0);             /* pduSessionId */
	gbinder_writer_append_int32(&w, 0);             /* sliceInfo: null */
	gbinder_writer_append_bool(&w, TRUE);           /* matchAllRuleAllowed */
	send_req(DATA, DATA_setupDataCall, r);
	printf("data: setting up a call on APN \"%s\" (%s)\n", apn, rat_name(st.data_rat));
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

/* ---------------------------------------------------------------------------------------- */

static gboolean read_info(GBinderReader *r, int *type, int *serial, int *error)
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
	*type = t; *serial = s; *error = e;
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
	for (i = 0; i < napps && i < 16; i++) {
		gint32 type = 0, state = 0, perso = 0, pin1r, pin1 = 0, pin2;
		char *aid = NULL;
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
		printf("sim: app %d: type %d (%s), state %d (%s), pin1 %d\n", i, type,
		       type == 2 ? "USIM" : type == 1 ? "SIM" : type == 5 ? "ISIM" : "other", state,
		       state == 5 ? "ready" : state == 2 ? "PIN required" : state == 3 ? "PUK required" :
		       state == 4 ? "network locked" : state == 1 ? "detected" : "unknown", pin1);
		if (i == gsm_idx || (gsm_idx < 0 && i == 0)) {
			st.app_type = type;
			st.app_state = state;
			g_free(st.aid);
			st.aid = aid;
			aid = NULL;
		}
		g_free(aid);
	}
	gbinder_reader_finish_parcelable(&p);
	printf("sim: card %s, %d application(s)\n",
	       card == 0 ? "ABSENT" : card == 1 ? "present" : card == 2 ? "ERROR" :
	       card == 3 ? "restricted" : "?", napps);
}

static void read_reg_state(GBinderReader *r, int *reg, int *rat, const char *what)
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
	if (plmn && *plmn)
		g_strlcpy(st.plmn, plmn, sizeof st.plmn);
	printf("%s: %s, %s%s%s\n", what, reg_name(g), rat_name(t),
	       plmn && *plmn ? ", network " : "", plmn && *plmn ? plmn : "");
	if (g == 3)
		printf("%s: denial reason %d\n", what, denial);
	g_free(plmn);
}

static void read_signal(GBinderReader *r)
{
	GBinderReader p, q;
	gboolean nn;
	gint32 v[7], i;
	if (!gbinder_reader_start_parcelable(r, &p, &nn) || !nn)
		return;
	gbinder_reader_skip_parcelable(&p);            /* gsm */
	gbinder_reader_skip_parcelable(&p);            /* cdma */
	gbinder_reader_skip_parcelable(&p);            /* evdo */
	if (gbinder_reader_start_parcelable(&p, &q, &nn) && nn) {  /* lte */
		for (i = 0; i < 7; i++)
			gbinder_reader_read_int32(&q, &v[i]);
		gbinder_reader_finish_parcelable(&q);
		st.lte_rsrp = v[1];
		st.lte_rsrq = v[2];
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

static void print_signal(void)
{
	if (st.lte_rsrp != 0x7fffffff && st.lte_rsrp > 0)
		printf("signal: LTE RSRP -%d dBm, RSRQ -%d dB\n", st.lte_rsrp, st.lte_rsrq);
	if (st.nr_rsrp != 0x7fffffff && st.nr_rsrp > 0)
		printf("signal: NR SS-RSRP -%d dBm\n", st.nr_rsrp);
	if (!(st.lte_rsrp > 0 && st.lte_rsrp != 0x7fffffff) && !(st.nr_rsrp > 0 && st.nr_rsrp != 0x7fffffff))
		printf("signal: no LTE/NR measurement\n");
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

static void run(const char *cmd)
{
	int rc = system(cmd);
	printf("  # %s%s\n", cmd, rc ? "  (failed)" : "");
}

/* S24_RADIO_HOOK (set by the s24-mobile-data service): "HOOK up IFACE DNS..." once the call is
 * configured, "HOOK down IFACE" once it is gone; it handles DNS and IPv4 (CLAT) */
static void run_hook(const char *what, const char *ifname, char **dnses)
{
	const char *hook = getenv("S24_RADIO_HOOK");
	GString *cmd;
	int i;
	if (!hook || !*hook)
		return;
	cmd = g_string_new(hook);
	g_string_append_printf(cmd, " %s %s", what, ifname && *ifname ? ifname : "-");
	for (i = 0; dnses && dnses[i]; i++) {
		gchar *q = g_shell_quote(dnses[i]);
		g_string_append_printf(cmd, " %s", q);
		g_free(q);
	}
	run(cmd->str);
	g_string_free(cmd, TRUE);
}

static void configure_link(const char *ifname, char **addrs, char **dnses, char **gws, int mtu)
{
	char cmd[512];
	gboolean has_v4 = FALSE;
	int i;
	g_strlcpy(st.ifname, ifname, sizeof st.ifname);
	snprintf(cmd, sizeof cmd, "ip addr flush dev %s", ifname);
	run(cmd);
	for (i = 0; addrs[i]; i++) {
		const char *a = addrs[i];
		gboolean v6 = strchr(a, ':') != NULL;
		snprintf(cmd, sizeof cmd, "ip %s addr add %s%s dev %s", v6 ? "-6" : "-4", a,
		         strchr(a, '/') ? "" : (v6 ? "/64" : "/32"), ifname);
		run(cmd);
		has_v4 |= !v6;
	}
	if (mtu > 0) {
		snprintf(cmd, sizeof cmd, "ip link set dev %s mtu %d", ifname, mtu);
		run(cmd);
	}
	snprintf(cmd, sizeof cmd, "ip link set dev %s up", ifname);
	run(cmd);
	/* default routes that lose to Wi-Fi (NetworkManager uses metric 600 for it); IPv4 only when
	 * the call has an IPv4 address (an IPv6-only network gets IPv4 through the hook's CLAT) */
	if (has_v4) {
		snprintf(cmd, sizeof cmd, "ip -4 route replace default dev %s metric 900", ifname);
		run(cmd);
	}
	snprintf(cmd, sizeof cmd, "ip -6 route replace default dev %s metric 900", ifname);
	run(cmd);
	for (i = 0; dnses[i]; i++)
		printf("data: DNS %s\n", dnses[i]);
	for (i = 0; gws[i]; i++)
		printf("data: gateway %s\n", gws[i]);
	fflush(stdout);
	run_hook("up", ifname, dnses);
}

/* SetupDataCallResult; returns cid or -1 */
static int read_dc_result(GBinderReader *r, gboolean apply)
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
	printf("data: call cid %d, cause %d, %s, interface %s, %d address(es), mtu %d/%d\n", cid,
	       cause, active == 2 ? "active" : active == 1 ? "dormant" : "inactive",
	       ifname ? ifname : "-", n, mtu4, mtu6);
	for (i = 0; addrs[i]; i++)
		printf("data: address %s\n", addrs[i]);
	if (apply && cause == 0 && active && ifname && *ifname && n > 0) {
		configure_link(ifname, addrs, dnses, gws, mtu4 > 0 ? mtu4 : mtu6);
		st.cid = cid;
	}
	g_free(ifname);
	g_strfreev(addrs);
	g_strfreev(dnses);
	g_strfreev(gws);
	g_strfreev(pcscf);
	return cause == 0 ? cid : -1;
}

/* ---------------------------------------------------------------------------------------- */

static void status_requests(void)
{
	call_serial(SIM, SIM_getIccCardStatus);
	call_serial(MODEM, MODEM_getBasebandVersion);
	call_serial(NET, NET_getVoiceRegistrationState);
	call_serial(NET, NET_getDataRegistrationState);
	call_serial(NET, NET_getOperator);
	call_serial(NET, NET_getSignalStrength);
	call_serial(NET, NET_getAllowedNetworkTypesBitmap);
	call_serial(NET, NET_getUsageSetting);
	call_serial(NET, NET_getImsRegistrationState);
	call_serial(DATA, DATA_getDataCallList);
}

/* MNC length by MCC: Android's fallback when the SIM doesn't say (EF_AD); these use 3 digits */
static int mnc_digits(const char *imsi)
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

static void get_imsi(void)
{
	GBinderWriter w;
	GBinderLocalRequest *r = req_new(SIM, &w);
	gbinder_writer_append_int32(&w, next_serial++);
	gbinder_writer_append_string16(&w, st.aid ? st.aid : "");
	send_req(SIM, SIM_getImsiForApp, r);
}

/* "data auto": the APN from Samsung's own list in the image (apns-conf.xml), the way Android
 * picks it: entries for the SIM's MCC/MNC whose type covers "default"; an MVNO entry
 * (mvno_type="imsi", digits with x wildcards) wins when the IMSI matches. MVNO entries matched by
 * SPN/GID/ICCID can't be checked here and are skipped. */
#define APN_LIST "/android/system/etc/apns-conf.xml"
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
	const char *mcc = NULL, *mnc = NULL, *apn_ = NULL, *type = NULL, *mvt = NULL, *mvd = NULL;
	const char *carrier = NULL;
	int i, prio;
	if (strcmp(el, "apn"))
		return;
	for (i = 0; names[i]; i++) {
		if (!strcmp(names[i], "mcc")) mcc = vals[i];
		else if (!strcmp(names[i], "mnc")) mnc = vals[i];
		else if (!strcmp(names[i], "apn")) apn_ = vals[i];
		else if (!strcmp(names[i], "type")) type = vals[i];
		else if (!strcmp(names[i], "mvno_type")) mvt = vals[i];
		else if (!strcmp(names[i], "mvno_match_data")) mvd = vals[i];
		else if (!strcmp(names[i], "carrier")) carrier = vals[i];
	}
	if (!mcc || !mnc || !apn_ || strcmp(mcc, p->mcc) || strcmp(mnc, p->mnc))
		return;
	if (type && !strstr(type, "default") && strcmp(type, "*"))
		return;
	if (mvt) {
		if (strcmp(mvt, "imsi") || !mvd || !imsi_matches(mvd, p->imsi))
			return;
		prio = 2;
	} else {
		prio = 1;
	}
	if (prio > p->prio) {
		g_free(p->apn);
		g_free(p->carrier);
		p->apn = g_strdup(apn_);
		p->carrier = g_strdup(carrier ? carrier : "?");
		p->prio = prio;
	}
}

static void pick_apn(const char *imsi, int mnc_len)
{
	static const GMarkupParser parser = { apn_element, NULL, NULL, NULL, NULL };
	ApnPick p = { .imsi = imsi };
	gchar *xml = NULL;
	gsize len = 0;
	GMarkupParseContext *c;
	memcpy(p.mcc, imsi, 3);
	memcpy(p.mnc, imsi + 3, mnc_len);
	if (g_file_get_contents(APN_LIST, &xml, &len, NULL)) {
		c = g_markup_parse_context_new(&parser, 0, &p, NULL);
		g_markup_parse_context_parse(c, xml, len, NULL);   /* keeps what it found on errors */
		g_markup_parse_context_free(c);
		g_free(xml);
	}
	if (p.apn) {
		printf("data: APN \"%s\" from the phone's APN list (%s)\n", p.apn, p.carrier);
		apn = p.apn;                                         /* kept for the process */
	} else {
		printf("data: no APN for %s%s in the phone's list: using the network's default\n",
		       p.mcc, p.mnc);
		apn = "";
	}
	g_free(p.carrier);
}

/* What Android's phone framework does once it has read the SIM: publish the SIM's operator
 * (gsm.sim.operator.numeric, one comma-separated entry per slot). Samsung's RIL reads it when the
 * initial attach APN arrives (DataCallManager::GetSimOperator -> LoadSimOperator) and drops the APN
 * without it ("DCM-SkipSetInitialAttachApn(): invalid sim operator"); without that APN the modem
 * never attaches to LTE and stays on 2G. */
static void publish_sim_operator(void)
{
	const char *slot = getenv("S24_SLOT");
	int idx = slot && atoi(slot) == 2 ? 1 : 0;
	gchar *out = NULL, **v, *val, *cmd;
	const gchar *e[2];
	if (!g_spawn_command_line_sync("chroot /android /system/bin/getprop gsm.sim.operator.numeric",
	                               &out, NULL, NULL, NULL))
		out = NULL;
	v = g_strsplit(out ? g_strstrip(out) : "", ",", 2);
	e[0] = v[0] ? v[0] : "";
	e[1] = v[0] && v[1] ? v[1] : "";
	e[idx] = st.imsi_plmn;
	val = g_strjoin(",", e[0], e[1], NULL);
	cmd = g_strdup_printf("chroot /android /system/bin/setprop gsm.sim.operator.numeric %s", val);
	run(cmd);
	g_free(cmd);
	g_free(val);
	g_strfreev(v);
	g_free(out);
}

/* data mode, in Android's order: SIM -> IMSI -> publish the operator -> attach APN -> allow data
 * -> radio on -> registered (LTE/5G preferred) -> data call */
enum { S_SIM, S_IMSI, S_ATTACH_APN, S_ALLOW, S_POWER, S_REGISTER, S_SETUP, S_UP };
static void data_advance(void);

static gboolean poll_registration(gpointer unused)
{
	/* on LTE/5G at once; on 2G/3G only after ~16 s, as the modem may still move up */
	if ((registered(st.data_reg) && (access_network(st.data_rat) >= 3 || reg_tries >= 8)) ||
	    ++reg_tries > 60) {
		reg_timer = 0;
		if (!registered(st.data_reg)) {
			printf("data: not registered for data after 2 minutes, giving up\n");
			exit_code = 2;
			g_main_loop_quit(loop);
		} else {
			data_advance();
		}
		return G_SOURCE_REMOVE;
	}
	call_serial(NET, NET_getDataRegistrationState);
	return G_SOURCE_CONTINUE;
}

static void data_advance(void)
{
	switch (step) {
	case S_SIM:
		if (st.card == 0) {
			printf("data: no SIM in this slot\n");
			exit_code = 2;
			g_main_loop_quit(loop);
		} else if (st.app_state == 5 && st.aid) {
			step = S_IMSI;
			get_imsi();
		}           /* not ready (PIN?): simStatusChanged asks again */
		break;
	case S_IMSI:        /* the IMSI answer is in */
		step = S_ATTACH_APN;
		set_initial_attach_apn();
		break;
	case S_ATTACH_APN:
		step = S_ALLOW;
		set_data_allowed(TRUE);
		break;
	case S_ALLOW:
		step = S_POWER;
		set_radio_power(TRUE);
		break;
	case S_POWER:
		if (st.radio == 10) {
			step = S_REGISTER;
			reg_tries = 0;
			call_serial(NET, NET_getDataRegistrationState);
			reg_timer = g_timeout_add_seconds(2, poll_registration, NULL);
		}
		break;
	case S_REGISTER:
		if (!reg_timer) {   /* poll_registration has decided */
			step = S_SETUP;
			setup_data_call();
		}
		break;
	}
}

static void print_net_types(int b)
{
	static const struct { int bits; const char *name; } raf[] = {
		{ 65536, "GSM" }, { 2, "GPRS" }, { 4, "EDGE" }, { 8, "UMTS" }, { 512, "HSDPA" },
		{ 1024, "HSUPA" }, { 2048, "HSPA" }, { 32768, "HSPA+" }, { 16384, "LTE" },
		{ 524288, "LTE-CA" }, { 1048576, "NR(5G)" }, { 131072, "TD-SCDMA" },
		{ 16 | 32 | 64 | 128 | 256 | 4096 | 8192, "CDMA/EVDO" },
	};
	unsigned i;
	printf("network: allowed types 0x%x:", b);
	for (i = 0; i < G_N_ELEMENTS(raf); i++)
		if (b & raf[i].bits)
			printf(" %s", raf[i].name);
	printf("\n");
}

static void set_allowed_types(int bitmap)
{
	GBinderWriter w;
	GBinderLocalRequest *r = req_new(NET, &w);
	gbinder_writer_append_int32(&w, next_serial++);
	gbinder_writer_append_int32(&w, bitmap);
	send_req(NET, NET_setAllowedNetworkTypesBitmap, r);
}

static void on_response(int s, guint code, GBinderReader *r)
{
	int type, serial, error;
	if (code == 1)                                  /* acknowledgeRequest(serial) */
		return;
	if (!read_info(r, &type, &serial, &error))
		return;
	if (type == 2)
		ack(s);
	if (error && !(s == MODEM && code == MODEMR_getBasebandVersion))
		printf("%s: response %u: %s (%d)\n", SVC[s].name, code, radio_err(error), error);
	switch (s) {
	case SIM:
		if (code == SIMR_getIccCardStatus && !error) {
			read_card_status(r);
			if (!strcmp(mode, "data"))
				data_advance();
			else if (!strcmp(mode, "status") && st.aid && st.app_state == 5)
				get_imsi();
		} else if (code == SIMR_getImsiForApp) {
			char *imsi = error ? NULL : gbinder_reader_read_string16(r);
			if (imsi && strlen(imsi) >= 6) {
				g_strlcpy(st.imsi_plmn, imsi, 3 + mnc_digits(imsi) + 1);
				printf("sim: home network %s (from the IMSI)\n", st.imsi_plmn);
				if (!strcmp(mode, "data")) {
					publish_sim_operator();
					if (!*apn || !strcmp(apn, "auto"))
						pick_apn(imsi, mnc_digits(imsi));
				}
			} else {
				printf("sim: no IMSI (%s): the modem may not get the attach APN\n",
				       radio_err(error));
			}
			if (imsi)
				memset(imsi, 0, strlen(imsi));   /* not kept: only MCC/MNC is */
			g_free(imsi);
			if (!strcmp(mode, "data"))
				data_advance();
		}
		break;
	case MODEM:
		if (code == MODEMR_getBasebandVersion && !error) {
			char *v = gbinder_reader_read_string16(r);
			printf("modem: baseband %s\n", v ? v : "?");
			g_free(v);
		} else if (code == MODEMR_setRadioPower) {
			printf("modem: radio power request done (%s)\n", radio_err(error));
			if (!strcmp(mode, "power"))
				g_main_loop_quit(loop);
			if (!strcmp(mode, "data") && !error) {
				st.radio = 10;  /* no radioStateChanged comes when it was on already */
				data_advance();
			}
		}
		break;
	case NET:
		if (code == NETR_getDataRegistrationState && !error) {
			read_reg_state(r, &st.data_reg, &st.data_rat, "data");
			if (!strcmp(mode, "data"))
				data_advance();
		} else if (code == NETR_getVoiceRegistrationState && !error) {
			read_reg_state(r, &st.voice_reg, &st.voice_rat, "voice");
		} else if (code == NETR_getOperator && !error) {
			char *l = gbinder_reader_read_string16(r);
			char *sh = gbinder_reader_read_string16(r);
			char *num = gbinder_reader_read_string16(r);
			printf("network: operator %s%s%s%s%s\n", l && *l ? l : "(none)",
			       sh && *sh ? " / " : "", sh && *sh ? sh : "",
			       num && *num ? ", " : "", num && *num ? num : "");
			g_free(l); g_free(sh); g_free(num);
		} else if (code == NETR_getSignalStrength && !error) {
			read_signal(r);
			print_signal();
		} else if (code == NETR_getAllowedNetworkTypesBitmap) {
			gint32 b = 0;
			if (!error && gbinder_reader_read_int32(r, &b))
				print_net_types(b);
			if (!strcmp(mode, "net"))
				g_main_loop_quit(loop);
		} else if (code == NETR_setAllowedNetworkTypesBitmap) {
			printf("network: setting allowed types: %s\n", radio_err(error));
			if (!strcmp(mode, "net"))
				call_serial(NET, NET_getAllowedNetworkTypesBitmap);
		} else if (code == NETR_getUsageSetting) {
			gint32 u = 0;
			if (!error && gbinder_reader_read_int32(r, &u))
				printf("network: usage setting %s (%d)\n", u == 1 ? "voice-centric" :
				       u == 2 ? "data-centric" : "?", u);
			if (!strcmp(mode, "usage"))
				g_main_loop_quit(loop);
		} else if (code == NETR_setUsageSetting) {
			printf("network: setting usage: %s\n", radio_err(error));
			if (!strcmp(mode, "usage"))
				call_serial(NET, NET_getUsageSetting);
		} else if (code == NETR_getImsRegistrationState && !error) {
			gint32 reg = 0, fam = 0;
			gbinder_reader_read_int32(r, &reg);
			gbinder_reader_read_int32(r, &fam);
			printf("network: IMS (VoLTE) %s\n", reg ? "registered" : "not registered");
		}
		break;
	case DATA:
		if (code == DATAR_getDataCallList && !error) {
			gint32 n = 0, i;
			gbinder_reader_read_int32(r, &n);
			printf("data: %d call(s) active\n", n);
			for (i = 0; i < n && i < 8; i++)
				read_dc_result(r, FALSE);
		} else if (code == DATAR_setInitialAttachApn) {
			if (step == S_ATTACH_APN)
				data_advance();
		} else if (code == DATAR_setDataAllowed) {
			if (step == S_ALLOW)
				data_advance();
		} else if (code == DATAR_setupDataCall) {
			if (error || read_dc_result(r, TRUE) < 0) {
				printf("data: setup failed\n");
				exit_code = 3;
				g_main_loop_quit(loop);
			} else {
				step = S_UP;
				printf("data: up on %s (stop with Ctrl+C or SIGTERM)\n", st.ifname);
			}
		} else if (code == DATAR_deactivateDataCall) {
			printf("data: call released (%s)\n", radio_err(error));
			g_main_loop_quit(loop);
		}
		break;
	}
	fflush(stdout);
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
			printf("modem: radio %s\n", v == 10 ? "ON" : v == 0 ? "OFF" : "unavailable");
			if (!strcmp(mode, "data"))
				data_advance();
		} else if (code == MODEMI_rilConnected) {
			printf("modem: RIL connected\n");
		}
		break;
	case SIM:
		if (code == SIMI_simStatusChanged)
			call_serial(SIM, SIM_getIccCardStatus);
		break;
	case NET:
		if (code == NETI_networkStateChanged) {
			call_serial(NET, NET_getDataRegistrationState);
		} else if (code == NETI_currentSignalStrength && !strcmp(mode, "data") && step < S_UP) {
			read_signal(r);
		}
		break;
	case DATA:
		if (code == DATAI_dataCallListChanged && step == S_UP) {
			gint32 n = 0, i, found = 0;
			gbinder_reader_read_int32(r, &n);
			for (i = 0; i < n && i < 8; i++)
				if (read_dc_result(r, FALSE) == st.cid)
					found = 1;
			if (!found) {
				printf("data: the network dropped the call\n");
				run_hook("down", st.ifname, NULL);
				exit_code = 4;
				st.cid = -1;
				g_main_loop_quit(loop);
			}
		}
		break;
	}
	fflush(stdout);
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

static gboolean on_signal(gpointer unused)
{
	if (st.cid >= 0) {
		run_hook("down", st.ifname, NULL);
		printf("data: releasing call %d\n", st.cid);
		fflush(stdout);
		deactivate_data_call(st.cid);
		st.cid = -1;
		g_timeout_add_seconds(5, (GSourceFunc)g_main_loop_quit, loop);
	} else {
		g_main_loop_quit(loop);
	}
	return G_SOURCE_CONTINUE;
}

static gboolean on_timeout(gpointer unused)
{
	g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}

static int connect_all(void)
{
	int s, i;
	sm = gbinder_servicemanager_new("/dev/binder");
	if (!sm) {
		fprintf(stderr, "s24-radio: no service manager on /dev/binder\n");
		return -1;
	}
	for (s = 0; s < NSVC; s++) {
		const char *slot = getenv("S24_SLOT");  /* SIM slot: 1 (default) or 2 */
		char *fq = s == CFG || !slot ? g_strdup_printf("%s/%s", SVC[s].iface, SVC[s].instance) :
		           g_strdup_printf("%s/slot%s", SVC[s].iface, slot);
		int status = 0;
		GBinderRemoteObject *o = gbinder_servicemanager_get_service_sync(sm, fq, &status);
		if (!o) {
			fprintf(stderr, "s24-radio: %s not found (%d): is rild running?\n", fq, status);
			g_free(fq);
			return -1;
		}
		remote[s] = gbinder_remote_object_ref(o);
		client[s] = gbinder_client_new(remote[s], SVC[s].iface);
		g_free(fq);
		for (i = 0; i < 2; i++) {
			char *iface = g_strconcat(SVC[s].iface, i ? "Indication" : "Response", NULL);
			cb_kind[s][i].svc = s;
			cb_kind[s][i].indication = i;
			cb_obj[s][i] = gbinder_servicemanager_new_local_object(sm, iface, on_transact,
			                                                       &cb_kind[s][i]);
			gbinder_local_object_set_stability(cb_obj[s][i], GBINDER_STABILITY_VINTF);
			g_free(iface);
		}
		{
			GBinderWriter w;
			GBinderLocalRequest *r = req_new(s, &w);
			gbinder_writer_append_local_object(&w, cb_obj[s][0]);
			gbinder_writer_append_local_object(&w, cb_obj[s][1]);
			if (send_req(s, SVC[s].set_resp, r))
				return -1;
		}
	}
	return 0;
}

/* rild answers only the last program that registered: running next to s24-mmd would take its
 * answers away (and leave rild calling a dead process once we exit) */
static gboolean mmd_running(void)
{
	GDir *d = g_dir_open("/proc", 0, NULL);
	const char *e;
	gboolean found = FALSE;
	while (d && !found && (e = g_dir_read_name(d))) {
		gchar *f, *comm = NULL;
		if (!g_ascii_isdigit(*e))
			continue;
		f = g_strdup_printf("/proc/%s/comm", e);
		if (g_file_get_contents(f, &comm, NULL, NULL) && !strcmp(g_strstrip(comm), "s24-mmd"))
			found = TRUE;
		g_free(comm);
		g_free(f);
	}
	if (d)
		g_dir_close(d);
	return found;
}

int main(int argc, char **argv)
{
	mode = argc > 1 ? argv[1] : "status";
	if (mmd_running() && !getenv("S24_RADIO_FORCE")) {
		fprintf(stderr, "s24-radio: s24-mmd is running and owns rild's answers: use mmcli/nmcli "
		        "(or stop s24-mmd; S24_RADIO_FORCE=1 overrides)\n");
		return 1;
	}
	if (!strcmp(mode, "power")) {
		if (argc < 3 || (strcmp(argv[2], "on") && strcmp(argv[2], "off"))) {
			fprintf(stderr, "usage: s24-radio power on|off\n");
			return 1;
		}
		want_power = !strcmp(argv[2], "on");
	} else if (!strcmp(mode, "data")) {
		apn = argc > 2 ? argv[2] : "";
	} else if (!strcmp(mode, "net")) {
		if (argc > 2) {
			want_types = !strcmp(argv[2], "all") ? RAF_ALL : (int)strtol(argv[2], NULL, 0);
			if (want_types <= 0) {
				fprintf(stderr, "usage: s24-radio net [all|0xBITS]\n");
				return 1;
			}
		}
	} else if (!strcmp(mode, "usage")) {
		if (argc > 2) {
			want_usage = !strcmp(argv[2], "voice") ? 1 : !strcmp(argv[2], "data") ? 2 : 0;
			if (!want_usage) {
				fprintf(stderr, "usage: s24-radio usage [voice|data]\n");
				return 1;
			}
		}
	} else if (strcmp(mode, "status")) {
		fprintf(stderr, "usage: s24-radio status | power on|off | data [APN] | net [all|0xBITS]"
		        " | usage [voice|data]\n");
		return 1;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	loop = g_main_loop_new(NULL, FALSE);
	if (connect_all())
		return 1;
	g_unix_signal_add(SIGINT, on_signal, NULL);
	g_unix_signal_add(SIGTERM, on_signal, NULL);
	if (!strcmp(mode, "status")) {
		status_requests();
		g_timeout_add_seconds(6, on_timeout, NULL);
	} else if (!strcmp(mode, "power")) {
		set_radio_power(want_power);
		g_timeout_add_seconds(15, on_timeout, NULL);
	} else if (!strcmp(mode, "net")) {
		if (want_types)
			set_allowed_types(want_types);
		else
			call_serial(NET, NET_getAllowedNetworkTypesBitmap);
		g_timeout_add_seconds(10, on_timeout, NULL);
	} else if (!strcmp(mode, "usage")) {
		if (want_usage) {
			GBinderWriter w;
			GBinderLocalRequest *r = req_new(NET, &w);
			gbinder_writer_append_int32(&w, next_serial++);
			gbinder_writer_append_int32(&w, want_usage);
			send_req(NET, NET_setUsageSetting, r);
		} else {
			call_serial(NET, NET_getUsageSetting);
		}
		g_timeout_add_seconds(10, on_timeout, NULL);
	} else {
		call_serial(SIM, SIM_getIccCardStatus);  /* its answer drives the rest (data_advance) */
	}
	g_main_loop_run(loop);
	return exit_code;
}
