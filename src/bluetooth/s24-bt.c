// Galaxy S24 (SM-S921B) Bluetooth for postmarketOS (Claude, 2026-11-15).
//
// The chip is a Qualcomm WCN7851 ("HMT", the Bluetooth half of the kiwi combo) on /dev/ttySAC1,
// powered by Samsung's btpower driver (/dev/btpower). This kernel's btqca (6.1) predates WCN7850
// support, so its firmware download is done here, in userspace, following upstream Linux btqca.c
// (v6.12) for QCA_WCN7850: version, baud switch, rampatch hmtbtfw20.tlv, board id, NVM
// hmtnv20.bXX, disable SoC logging, HCI reset, build info. The NVM enables in-band sleep and deep
// sleep as upstream does (the firmware does in-band sleep regardless: plain H4 can't drive it, it
// needs hci_uart's Qualcomm protocol); the BD address comes from EFS. Then the tty is handed to the kernel (N_HCI with
// Qualcomm's protocol, from the s24bt module) and this process stays alive holding it: hci0 lives
// as long as it runs.
//
//   s24-bt up [-s SPEED] [-f FWDIR] [-a ADDRFILE]   power on, load, attach, stay (foreground)
//   s24-bt power on|off
//
// Build on the phone: gcc -O2 -Wall -o s24-bt s24-bt.c
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <asm/termbits.h>
#ifndef TCGETS2 /* musl's <sys/ioctl.h> lacks them; asm-generic values */
#define TCGETS2 _IOR('T', 0x2A, struct termios2)
#define TCSETS2 _IOW('T', 0x2B, struct termios2)
#endif

#define BT_CMD_PWR_CTRL 0xbfad   /* Qualcomm btpower: arg 0 = off, 1 = on */
#define N_HCI 15
#define HCIUARTSETPROTO _IOW('U', 200, int)
#define HCIUARTGETDEVICE _IOR('U', 202, int)
#define HCIUARTSETFLAGS _IOW('U', 203, int)
#define HCI_UART_H4 0
#define HCI_UART_QCA 8

#define EDL_PATCH_CMD_OPCODE 0xFC00
#define EDL_WRITE_BD_ADDR_OPCODE 0xFC14
#define QCA_DISABLE_LOGGING 0xFC17
#define EDL_PATCH_VER_REQ_CMD 0x19
#define EDL_PATCH_TLV_REQ_CMD 0x1E
#define EDL_GET_BUILD_INFO_CMD 0x20
#define EDL_GET_BID_REQ_CMD 0x23
#define MAX_SEG 243
#define TAG_BD_ADDR 2
#define TAG_HCI 17
#define TAG_DEEP_SLEEP 27

static const char *tty_path = "/dev/ttySAC1";
static const char *pwr_path = "/dev/btpower";
static const char *fwdir = "/vendor/firmware";
static const char *addr_path = "/mnt/vendor/efs/bluetooth/bt_addr";
static int fd = -1, pfd = -1;

static void die(const char *m)
{
	fprintf(stderr, "s24-bt: %s: %s\n", m, strerror(errno));
	exit(1);
}

static long now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

static int baud_index(unsigned s)
{
	static const unsigned tab[] = { 115200, 57600, 38400, 19200, 9600, 230400, 250000, 460800,
		500000, 720000, 921600, 1000000, 1250000, 2000000, 3000000, 4000000, 1600000, 3200000,
		3500000 };
	for (unsigned i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
		if (tab[i] == s)
			return i;
	return -1;
}

static void set_speed(unsigned speed, int flow)
{
	struct termios2 t;
	if (ioctl(fd, TCGETS2, &t) < 0)
		die("TCGETS2");
	t.c_iflag = 0;
	t.c_oflag = 0;
	t.c_lflag = 0;
	t.c_cflag &= ~(CBAUD | CSIZE | PARENB | CSTOPB | CRTSCTS);
	t.c_cflag |= BOTHER | CS8 | CLOCAL | CREAD | (flow ? CRTSCTS : 0);
	t.c_ispeed = t.c_ospeed = speed;
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	if (ioctl(fd, TCSETS2, &t) < 0)
		die("TCSETS2");
}

static void set_rts(int on)
{
	int b = TIOCM_RTS;
	ioctl(fd, on ? TIOCMBIS : TIOCMBIC, &b);
}

static void write_all(const uint8_t *p, size_t n)
{
	while (n) {
		ssize_t w = write(fd, p, n);
		if (w < 0) {
			if (errno == EINTR || errno == EAGAIN) {
				usleep(1000);
				continue;
			}
			die("write");
		}
		p += w;
		n -= w;
	}
}

static int read_n(uint8_t *p, size_t n, long deadline)
{
	while (n) {
		long left = deadline - now_ms();
		if (left <= 0)
			return -1;
		struct pollfd pf = { fd, POLLIN, 0 };
		if (poll(&pf, 1, left) <= 0)
			continue;
		ssize_t r = read(fd, p, n);
		if (r <= 0)
			continue;
		p += r;
		n -= r;
	}
	return 0;
}

/* one H4 packet; returns its type (4 = event) with the event in buf (code, plen, params), or -1 */
static int read_pkt(uint8_t *buf, int timeout_ms)
{
	long dl = now_ms() + timeout_ms;
	uint8_t t;
	for (;;) {
		if (read_n(&t, 1, dl) < 0)
			return -1;
		if (t == 0x04) {
			if (read_n(buf, 2, dl) < 0 || read_n(buf + 2, buf[1], dl) < 0)
				return -1;
			return 4;
		}
		if (t == 0x02) { /* ACL: skip */
			uint8_t h[4];
			if (read_n(h, 4, dl) < 0)
				return -1;
			int l = h[2] | h[3] << 8;
			uint8_t junk[1024];
			while (l > 0) {
				int c = l > (int)sizeof(junk) ? (int)sizeof(junk) : l;
				if (read_n(junk, c, dl) < 0)
					return -1;
				l -= c;
			}
			continue;
		}
		fprintf(stderr, "s24-bt: stray byte 0x%02x\n", t);
	}
}

static void send_cmd(uint16_t op, const uint8_t *p, uint8_t n)
{
	uint8_t b[260] = { 0x01, op & 0xff, op >> 8, n };
	memcpy(b + 4, p, n);
	write_all(b, 4 + n);
}

/* wait for Command Complete/Status of op (or, with vse, any vendor event); returns the params
 * after the opcode (CC) or the vendor event payload, length in *len; NULL on timeout */
static uint8_t evbuf[260];
static uint8_t *wait_ev(uint16_t op, int vse, int *len, int timeout_ms)
{
	long dl = now_ms() + timeout_ms;
	for (;;) {
		long left = dl - now_ms();
		if (left <= 0 || read_pkt(evbuf, left) < 0)
			return NULL;
		uint8_t code = evbuf[0], plen = evbuf[1], *pp = evbuf + 2;
		if (code == 0x0E && plen >= 3 && (pp[1] | pp[2] << 8) == op) {
			*len = plen - 3;
			return pp + 3;
		}
		if (code == 0x0F && plen >= 4 && (pp[2] | pp[3] << 8) == op) {
			fprintf(stderr, "s24-bt: command 0x%04x status 0x%02x\n", op, pp[0]);
			if (pp[0]) {
				*len = 1;
				return pp;
			}
			continue;
		}
		if (code == 0xFF && vse) {
			*len = plen;
			return pp;
		}
		fprintf(stderr, "s24-bt: (event 0x%02x, %d bytes skipped)\n", code, plen);
	}
}

static uint8_t *cmd(uint16_t op, const uint8_t *p, uint8_t n, int *len, int timeout_ms)
{
	send_cmd(op, p, n);
	uint8_t *r = wait_ev(op, 0, len, timeout_ms);
	if (!r)
		fprintf(stderr, "s24-bt: no answer to command 0x%04x\n", op);
	return r;
}

static uint8_t *load(const char *name, size_t *n)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", fwdir, name);
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *d = malloc(*n);
	if (fread(d, 1, *n, f) != *n)
		die("read firmware");
	fclose(f);
	printf("s24-bt: %s (%zu bytes)\n", path, *n);
	return d;
}

/* TLV download as qca_download_firmware(): 243-byte segments; with download mode 1/3 (skip VSE)
 * only the last (short) segment is acknowledged */
static int download(const uint8_t *d, size_t n, int mode)
{
	size_t off = 0;
	int seg = 0;
	while (off < n) {
		int s = n - off > MAX_SEG ? MAX_SEG : n - off;
		int last = off + s >= n || s < MAX_SEG;
		uint8_t b[MAX_SEG + 2] = { EDL_PATCH_TLV_REQ_CMD, s };
		memcpy(b + 2, d + off, s);
		send_cmd(EDL_PATCH_CMD_OPCODE, b, s + 2);
		if (!(mode == 1 || mode == 3) || last) {
			int l;
			uint8_t *r = wait_ev(EDL_PATCH_CMD_OPCODE, 1, &l, 3000);
			if (!r) {
				fprintf(stderr, "s24-bt: no ack for segment %d\n", seg);
				return -1;
			}
			if (l >= 1 && r[0] != 0) {
				fprintf(stderr, "s24-bt: segment %d answered 0x%02x 0x%02x\n", seg, r[0], l > 1 ? r[1] : 0);
				if (!(l >= 2 && r[0] == 0x00))
					; /* vendor event layouts vary; only the status byte is checked */
			}
		}
		off += s;
		seg++;
	}
	printf("s24-bt: %d segments sent\n", seg);
	return 0;
}

static int read_addr(uint8_t le[6])
{
	FILE *f = fopen(addr_path, "r");
	unsigned a[6];
	if (!f)
		return -1;
	int k = fscanf(f, "%x:%x:%x:%x:%x:%x", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]);
	fclose(f);
	if (k != 6)
		return -1;
	for (int i = 0; i < 6; i++)
		le[i] = a[5 - i];  /* bdaddr_t is little-endian: last octet first */
	return 0;
}

static void power(int on)
{
	if (pfd < 0 && (pfd = open(pwr_path, O_RDWR)) < 0)
		die(pwr_path);
	if (ioctl(pfd, BT_CMD_PWR_CTRL, (unsigned long)on) < 0)
		fprintf(stderr, "s24-bt: btpower %s: %s\n", on ? "on" : "off", strerror(errno));
}

static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

int main(int argc, char **argv)
{
	unsigned speed = 3000000;
	if (argc >= 3 && !strcmp(argv[1], "power")) {
		power(!strcmp(argv[2], "on"));
		return 0;
	}
	if (argc >= 2 && !strcmp(argv[1], "warm")) {
		/* a short power-on with one command over the UART, then off. Needed once after btpower
		 * loads: the first real start otherwise leaves its BT_host_wake wakeup source held for
		 * good (no deep sleep while Bluetooth is on); a start after this one doesn't */
		power(0);
		usleep(200000);
		power(1);
		usleep(200000);
		if ((fd = open(tty_path, O_RDWR | O_NOCTTY)) < 0)
			die(tty_path);
		set_speed(115200, 1);
		ioctl(fd, TCFLSH, TCIOFLUSH);
		uint8_t c = EDL_PATCH_VER_REQ_CMD;
		int l;
		printf("s24-bt: warm-up %s\n", cmd(EDL_PATCH_CMD_OPCODE, &c, 1, &l, 1000) ? "ok" : "got no answer");
		close(fd);
		power(0);
		return 0;
	}
	if (argc < 2 || strcmp(argv[1], "up")) {
		fprintf(stderr, "usage: s24-bt up [-s SPEED] [-f FWDIR] [-a ADDRFILE] | warm | power on|off\n");
		return 2;
	}
	for (int i = 2; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "-s"))
			speed = strtoul(argv[i + 1], NULL, 0);
		else if (!strcmp(argv[i], "-f"))
			fwdir = argv[i + 1];
		else if (!strcmp(argv[i], "-a"))
			addr_path = argv[i + 1];
	}
	int bidx = baud_index(speed);
	if (bidx < 0) {
		fprintf(stderr, "s24-bt: unsupported speed %u\n", speed);
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);

	/* power cycle: a clean ROM state for the patch download */
	power(0);
	usleep(200000);
	power(1);
	usleep(200000);

	if ((fd = open(tty_path, O_RDWR | O_NOCTTY)) < 0)
		die(tty_path);
	set_speed(115200, 1);
	ioctl(fd, TCFLSH, TCIOFLUSH);

	int l;
	uint8_t c = EDL_PATCH_VER_REQ_CMD, *r = NULL;
	for (int tries = 0; tries < 5 && !r; tries++)
		r = cmd(EDL_PATCH_CMD_OPCODE, &c, 1, &l, 1000);
	if (!r || l < 15) {
		fprintf(stderr, "s24-bt: no version answer from the chip\n");
		return 1;
	}
	/* CC params: status, 0x19, one byte, then product_id(4) patch_ver(2) rom_ver(2) soc_id(4) */
	uint32_t product = r[3] | r[4] << 8 | r[5] << 16 | (uint32_t)r[6] << 24;
	uint16_t patch = r[7] | r[8] << 8, rom = r[9] | r[10] << 8;
	uint32_t soc = r[11] | r[12] << 8 | r[13] << 16 | (uint32_t)r[14] << 24;
	uint32_t soc_ver = soc << 16 | rom;
	uint8_t rom_ver = ((soc_ver & 0xf00) >> 4) | (soc_ver & 0xf);
	printf("s24-bt: product 0x%08x soc 0x%08x rom 0x%04x patch 0x%04x -> rom_ver %02x\n",
	       product, soc, rom, patch, rom_ver);

	/* operational speed: controller first (host RTS held off), then the host */
	set_speed(115200, 0);
	set_rts(0);
	uint8_t bc[5] = { 0x01, 0x48, 0xFC, 0x01, bidx };
	write_all(bc, 5);
	ioctl(fd, TCSBRK, 1); /* tcdrain */
	usleep(10000);
	set_speed(speed, 1);
	set_rts(1);
	usleep(50000);
	r = cmd(EDL_PATCH_CMD_OPCODE, &c, 1, &l, 1000);
	if (!r) {
		fprintf(stderr, "s24-bt: chip silent at %u baud\n", speed);
		return 1;
	}
	printf("s24-bt: talking at %u baud\n", speed);

	char name[64];
	size_t n;
	snprintf(name, sizeof(name), "hmtbtfw%02x.tlv", rom_ver);
	uint8_t *fw = load(name, &n);
	if (!fw) {
		fprintf(stderr, "s24-bt: no %s in %s\n", name, fwdir);
		return 1;
	}
	int mode = n > 14 ? fw[4 + 10] : 0;  /* tlv_type_patch.download_mode */
	printf("s24-bt: rampatch download mode %d\n", mode);
	if (download(fw, n, mode) < 0)
		return 1;
	free(fw);
	usleep(10000);

	uint16_t bid = 0;
	c = EDL_GET_BID_REQ_CMD;
	r = cmd(EDL_PATCH_CMD_OPCODE, &c, 1, &l, 1000);
	if (r && l >= 5)
		bid = r[3] << 8 | r[4];
	if (bid == 0)
		snprintf(name, sizeof(name), "hmtnv%02x.bin", rom_ver);
	else if (bid & 0xff00)
		snprintf(name, sizeof(name), "hmtnv%02x.b%x", rom_ver, bid);
	else
		snprintf(name, sizeof(name), "hmtnv%02x.b%02x", rom_ver, bid);
	printf("s24-bt: board id 0x%x -> %s\n", bid, name);
	uint8_t *nv = load(name, &n);
	if (!nv) {
		fprintf(stderr, "s24-bt: no %s, trying hmtnv%02x.b20\n", name, rom_ver);
		snprintf(name, sizeof(name), "hmtnv%02x.b20", rom_ver);
		if (!(nv = load(name, &n)))
			return 1;
	}
	/* NVM tags (type 2, maybe inside a type-4 wrapper) */
	uint8_t addr[6];
	int have_addr = read_addr(addr) == 0;
	size_t off = 0;
	uint32_t tl = nv[0] | nv[1] << 8 | nv[2] << 16 | (uint32_t)nv[3] << 24;
	if ((tl & 0xff) == 4) {
		off = 4;
		tl = nv[4] | nv[5] << 8 | nv[6] << 16 | (uint32_t)nv[7] << 24;
	}
	if ((tl & 0xff) == 2) {
		size_t idx = off + 4, end = idx + (tl >> 8);
		while (idx + 12 <= end && idx + 12 <= n) {
			uint16_t id = nv[idx] | nv[idx + 1] << 8, tlen = nv[idx + 2] | nv[idx + 3] << 8;
			uint8_t *dd = nv + idx + 12;
			if (id == TAG_HCI && tlen >= 3) {
				dd[0] |= 0x80;    /* in-band sleep, as upstream btqca (this firmware uses it anyway) */
				dd[1] = bidx;     /* UART speed after the NVM applies */
			} else if (id == TAG_DEEP_SLEEP && tlen >= 1) {
				/* deep sleep when idle, as upstream btqca (S24BT_NODEEPSLEEP=1: off) */
				if (getenv("S24BT_NODEEPSLEEP"))
					dd[0] &= ~0x01;
				else
					dd[0] |= 0x01;
			} else if (id == TAG_BD_ADDR && tlen == 6 && have_addr) {
				memcpy(dd, addr, 6);
			}
			idx += 12 + tlen;
		}
	}
	printf("s24-bt: NVM patched (%s address)\n", have_addr ? "EFS" : "default");
	if (download(nv, n, 0) < 0)
		return 1;
	free(nv);

	uint8_t lg[2] = { 0x14, 0x00 };
	cmd(QCA_DISABLE_LOGGING, lg, 2, &l, 1000);
	r = cmd(0x0C03, NULL, 0, &l, 3000);
	if (!r || r[0]) {
		fprintf(stderr, "s24-bt: HCI reset failed\n");
		return 1;
	}
	c = EDL_GET_BUILD_INFO_CMD;
	r = cmd(EDL_PATCH_CMD_OPCODE, &c, 1, &l, 1000);
	if (r && l >= 3 && r[2] + 3 <= l)
		printf("s24-bt: firmware %.*s\n", r[2], (char *)r + 3);
	r = cmd(0x1009, NULL, 0, &l, 1000); /* Read BD_ADDR */
	if (r && l >= 7)
		printf("s24-bt: address %02X:%02X:%02X:xx:xx:xx\n", r[6], r[5], r[4]);

	if (getenv("S24BT_PROBE")) {  /* debug: the raw in-band-sleep exchange, host side played by hand */
		uint8_t b[256];
		static const char *what[] = { "idle 3 s", "host WAKE_IND 0xFD", "Read Local Version",
			"host WAKE_ACK 0xFC" };
		for (int phase = 0; phase < 4; phase++) {
			uint8_t one;
			if (phase == 0)
				usleep(3000000);
			else if (phase == 1)
				one = 0xFD, write_all(&one, 1);
			else if (phase == 2)
				send_cmd(0x1001, NULL, 0);
			else
				one = 0xFC, write_all(&one, 1);
			long dl = now_ms() + (phase == 0 ? 1000 : 600);
			int got = 0;
			printf("s24-bt: %s ->", what[phase]);
			while (now_ms() < dl) {
				struct pollfd pf = { fd, POLLIN, 0 };
				if (poll(&pf, 1, 20) <= 0)
					continue;
				ssize_t rr = read(fd, b, sizeof(b));
				for (ssize_t i = 0; i < rr; i++, got++)
					if (got < 48)
						printf(" %02x", b[i]);
			}
			printf(" (%d bytes)\n", got);
		}
		power(0);
		return 0;
	}

	/* hand the line to the kernel: hci_uart with Qualcomm's protocol (H4 plus in-band sleep:
	 * the chip announces sleep with 0xFE and wants a 0xFD/0xFC wake handshake; plain H4 can't).
	 * The s24bt module's hci_qca skips its own firmware setup for a line-discipline attach. */
	int ld = N_HCI, proto = getenv("S24BT_H4") ? HCI_UART_H4 : HCI_UART_QCA;
	if (ioctl(fd, TIOCSETD, &ld) < 0)
		die("TIOCSETD N_HCI (is hci_uart loaded?)");
	if (ioctl(fd, HCIUARTSETFLAGS, 0) < 0)
		die("HCIUARTSETFLAGS");
	if (ioctl(fd, HCIUARTSETPROTO, proto) < 0)
		die("HCIUARTSETPROTO");
	int dev = ioctl(fd, HCIUARTGETDEVICE, 0);
	printf("s24-bt: attached as hci%d, staying up\n", dev);

	signal(SIGTERM, on_sig);
	signal(SIGINT, on_sig);
	signal(SIGHUP, SIG_IGN);
	while (!stop)
		pause();
	close(fd);  /* detaches hci_uart: hci0 goes away */
	power(0);
	printf("s24-bt: stopped\n");
	return 0;
}
