/*
 * Curtana/miatoll touch-bus probe, compiled into the shim only when
 * TOUCH_PROBE=1. Runs with the MMU off (Device memory), no policy:
 *
 *   - reports the TLMM levels of reset/irq and of the four QUP1-SE pads
 *   - powers the touch VDDIO rail (pm6150 ldoa18, 1.8 V) through a
 *     conservative shim port of the kernel RPMh client (xnu 0097)
 *   - bit-bangs the FocalTech SPI frame format on the pad group that
 *     the live FDT assigns to spi@a94000 (gpio 53..56, function qup15)
 *     in all four plausible pin orders, one compact line per order
 *
 * TLMM layout (xnu 0089-sweet-touch-tlmm-tiles, verified live): the atoll
 * TLMM is split into tiles and gpio N lives at DT base + tile + N*0x1000.
 * Reset/IRQ (8/9) are in NORTH, the QUP1 pads 53..56 in WEST. Accessing
 * the bare DT base (0x03400000) hangs (shim probe v2, red screen stop).
 *
 * The shim then waits 90 s and resets. Nothing is written to disk.
 */
#include <stdint.h>
#include "console.h"

#if defined(TOUCH_PROBE) && TOUCH_PROBE

#define TLMM_NORTH	0x03900000UL	/* gpio 8 (rst), 9 (irq) */
#define TLMM_WEST	0x03500000UL	/* gpio 53..56 (QUP1 SE11 SPI pads) */

static inline uintptr_t pin_base(unsigned n)
{
	return (n == 8 || n == 9) ? TLMM_NORTH : TLMM_WEST;
}
#define GPIO_CFG(n)	(pin_base(n) + (uintptr_t)(n) * 0x1000UL)
#define GPIO_IO(n)	(GPIO_CFG(n) + 4UL)
#define GPIO_OUT	(1u << 1)
#define GPIO_OE		(1u << 9)

#define PIN_RESET	8
#define PIN_IRQ		9

#define FTS_WRITE_CMD	0x00
#define FTS_READ_CMD	0xa0	/* READ | DATA_CRC_EN */
#define FTS_HDR		4
#define FTS_DUMMY	3
#define FTS_DP		(FTS_HDR + FTS_DUMMY)

#define FTS_APP_A3	0x87	/* FocalTech app-mode chip id hi */
#define FTS_APP_9F	0x56	/* ... and lo */

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static uint64_t
now(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static uint64_t
cntfrq(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}

static void
delay_us(unsigned us)
{
	uint64_t t0 = now();
	uint64_t ticks = cntfrq() / 1000000u * us;
	while (now() - t0 < ticks)
		;
}

static int
gpio_level(unsigned pin)
{
	return rd32(GPIO_IO(pin)) & 1u;
}

static void
gpio_input(unsigned pin, unsigned pull)
{
	uint32_t c = rd32(GPIO_CFG(pin)) & ~0x3ffu;
	wr32(GPIO_CFG(pin), c | pull);	/* func 0, OE 0, pull 0/1/3 */
	(void)rd32(GPIO_CFG(pin));
}

static void
gpio_out(unsigned pin, int val, unsigned drive)
{
	uint32_t io = rd32(GPIO_IO(pin)) & ~GPIO_OUT;
	wr32(GPIO_IO(pin), val ? io | GPIO_OUT : io);
	uint32_t c = rd32(GPIO_CFG(pin)) & ~0x3ffu;
	wr32(GPIO_CFG(pin), c | (drive << 6) | GPIO_OE);
	(void)rd32(GPIO_CFG(pin));
}

static void
gpio_set(unsigned pin, int val)
{
	uint32_t io = rd32(GPIO_IO(pin)) & ~GPIO_OUT;
	wr32(GPIO_IO(pin), val ? io | GPIO_OUT : io);
}

struct bus {
	unsigned mosi, miso, clk, cs;
};

static void
bus_init(const struct bus *b)
{
	gpio_out(b->cs, 1, 6);
	gpio_out(b->clk, 0, 6);
	gpio_out(b->mosi, 0, 6);
	gpio_input(b->miso, 3);		/* panel usually pulls MISO up */
	delay_us(50);
}

/* One CS-framed full-duplex transfer, SPI mode 0, ~500 kHz. */
static void
spi_frame(const struct bus *b, const uint8_t *tx, uint8_t *rx, unsigned n)
{
	gpio_set(b->cs, 0);
	delay_us(1);
	for (unsigned i = 0; i < n; ++i) {
		uint8_t in = 0;
		for (int k = 7; k >= 0; --k) {
			gpio_set(b->mosi, (tx[i] >> k) & 1u);
			delay_us(1);
			gpio_set(b->clk, 1);
			delay_us(1);
			in = (uint8_t)((in << 1) | (gpio_level(b->miso) & 1u));
			gpio_set(b->clk, 0);
		}
		if (rx) {
			rx[i] = in;
		}
	}
	gpio_set(b->cs, 1);
	delay_us(150);			/* FTS CS high time (vendor: 150 us) */
}

static uint16_t
crc_kermit(const uint8_t *p, unsigned n)
{
	uint16_t crc = 0xffff;
	for (unsigned i = 0; i < n; ++i) {
		crc ^= p[i];
		for (int j = 0; j < 8; ++j) {
			crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
		}
	}
	return crc;
}

/* FTS write: [reg][0x00][len_hi][len_lo][3 dummy][data]. */
static int
fts_write(const struct bus *b, uint8_t reg, const uint8_t *data, unsigned len)
{
	uint8_t tx[96] = {0}, rx[96];
	tx[0] = reg;
	tx[1] = FTS_WRITE_CMD;
	tx[2] = (uint8_t)(len >> 8);
	tx[3] = (uint8_t)len;
	for (unsigned i = 0; i < len; ++i) {
		tx[FTS_DP + i] = data[i];
	}
	spi_frame(b, tx, rx, FTS_DP + len);
	return (rx[3] & 0xa0u) ? -1 : 0;
}

/* FTS read with CRC: [reg][0xa0][len_hi][len_lo][3 dummy][data][crc16]. */
static int
fts_read(const struct bus *b, uint8_t reg, uint8_t *out, unsigned len,
    int *crc_ok, uint8_t *status)
{
	uint8_t tx[96] = {0}, rx[96];
	tx[0] = reg;
	tx[1] = FTS_READ_CMD;
	tx[2] = (uint8_t)(len >> 8);
	tx[3] = (uint8_t)len;
	spi_frame(b, tx, rx, FTS_DP + len + 2);
	*status = rx[3];
	if (rx[3] & 0xa0u) {
		*crc_ok = -1;
		return -1;
	}
	for (unsigned i = 0; i < len; ++i) {
		out[i] = rx[FTS_DP + i];
	}
	uint16_t got = (uint16_t)(rx[FTS_DP + len] | (rx[FTS_DP + len + 1] << 8));
	*crc_ok = (crc_kermit(out, len) == got) ? 1 : 0;
	return 0;
}

static void
chip_reset(void)
{
	gpio_out(PIN_RESET, 0, 6);
	delay_us(2000);
	gpio_out(PIN_RESET, 1, 6);
	delay_us(3000);
}

/* ---- compact hex helpers: the operator types what the screen shows ---- */
static char pbuf[90];

static char *
scpy(char *p, const char *s)
{
	while (*s)
		*p++ = *s++;
	return p;
}

static char *
shex(char *p, uint32_t v, unsigned nd)
{
	static const char H[] = "0123456789ABCDEF";
	for (unsigned i = 0; i < nd; ++i)
		p[i] = H[(v >> (4 * (nd - 1 - i))) & 0xf];
	return p + nd;
}

static char *
pbuf0(char *p, const char *s)
{
	return scpy(p, s);
}

/* Read the boot id (0x55 0xaa handshake, then reg 0x90) and the app ids.
 * One compact line per order; returns 1 if a live FTS app chip was seen. */
static int
try_bus(const char *tag, const struct bus *b)
{
	uint8_t id[2] = {0}, a3 = 0, af = 0;
	int crc = 0, wr, rd, ra, rf;
	uint8_t stdb = 0, sa3 = 0, s9f = 0;

	bus_init(b);
	chip_reset();
	delay_us(250000);	/* FTS needs >100 ms with app warm to answer */

	const uint8_t start2 = 0xaa;
	wr = fts_write(b, 0x55, &start2, 1);
	delay_us(50000);
	rd = fts_read(b, 0x90, id, 2, &crc, &stdb);
	crc = 0;
	ra = fts_read(b, 0xa3, &a3, 1, &crc, &sa3);
	crc = 0;
	rf = fts_read(b, 0x9f, &af, 1, &crc, &s9f);
	(void)sa3; (void)s9f;

	/* FTS prints removed: the chip answered as Novatek (probe v9), so
	 * only the NVT dumps belong on screen now. The FTS protocol writes
	 * still run above (cheap sanity calls); no dump lines for them. */
	(void)a3; (void)af;
	return (ra == 0 && a3 == FTS_APP_A3 && rf == 0 && af == FTS_APP_9F);
}

/* ---- Vendor Novatek protocol (xnu 0094): write [reg|0x80][data],
 * read [reg&0x7f][dummy][data]. nvt_try() soft-resets via the swrst
 * register (GPIO 8 first goes back to input: the DT may share the reset
 * line with the panel) and dumps the chip id + raw header bytes. */
#define NVT_SWRST_N8	0x3f0feu	/* DT novatek,swrst-n8-addr */
#define NVT_RD_FAST	0x3f310u	/* DT novatek,spi-rd-fast-addr */
#define NVT_ID_PAGE	0x1f64eu	/* page holding the chip id */
#define NVT_ID_REG	0x4eu

static void
nvt_write(const struct bus *b, uint8_t reg, const uint8_t *d, unsigned len)
{
	uint8_t tx[8] = {0};
	tx[0] = reg | 0x80u;
	for (unsigned i = 0; i < len; ++i) {
		tx[1 + i] = d[i];
	}
	spi_frame(b, tx, 0, 1u + len);
}

static void
nvt_read(const struct bus *b, uint8_t reg, uint8_t *rx, unsigned len)
{
	uint8_t tx[4] = {0};
	tx[0] = reg & 0x7fu;
	/* data starts at rx[2]: one dummy byte after the register byte */
	spi_frame(b, tx, rx, 2u + len);
}

static void
nvt_read2(const struct bus *b, uint8_t reg, uint8_t *rx, unsigned len)
{
	uint8_t tx[4] = {0};
	tx[0] = reg & 0x7fu;
	/* alignment variant: one extra dummy; data assumed at rx[3] */
	spi_frame(b, tx, rx, 3u + len);
}

static void
nvt_page(const struct bus *b, uint32_t addr)
{
	uint8_t pg[2] = {(uint8_t)(addr >> 15), (uint8_t)(addr >> 7)};
	nvt_write(b, 0xff, pg, 2);
}

/* One compact line per order: swrst x3 + settle + id read, then dump the
 * RAW received bytes for the three frame alignments (pre=1..3 dummy/echo
 * bytes before the data). Spaced bytes so photos stay readable. */
static void
nvt_try(const char *tag, const struct bus *b)
{
	uint8_t rx[16] = {0}, one = 0x69, zero = 0x00;

	/* Do not drive GPIO 8 for the NVT pass; software reset only. */
	gpio_input(PIN_RESET, 1);
	gpio_input(PIN_IRQ, 3);
	bus_init(b);
	delay_us(50000);

	for (unsigned i = 0; i < 3; ++i) {
		nvt_page(b, NVT_SWRST_N8);
		nvt_write(b, (uint8_t)(NVT_SWRST_N8 & 0x7fu), &one, 1);
		delay_us(5000);
	}
	nvt_page(b, NVT_RD_FAST);
	nvt_write(b, (uint8_t)(NVT_RD_FAST & 0x7fu), &zero, 1);
	nvt_page(b, NVT_ID_PAGE);
	delay_us(250000);

	static const char tags[] = "123";
	for (unsigned pre = 1; pre <= 3; ++pre) {
		for (unsigned k = 0; k < 10; ++k) {
			rx[k] = 0;
		}
		uint8_t tx[8] = {0};
		tx[0] = NVT_ID_REG;
		spi_frame(b, tx, rx, pre + 6);
		char *p = pbuf0(pbuf, tag);
		p = scpy(p, " nv");
		*p++ = tags[pre - 1];
		*p++ = '=';
		for (unsigned k = 0; k < 6; ++k) {
			p = shex(p, rx[k], 2);
			*p++ = ' ';
		}
		*p = 0;
		con_str(tag, pbuf);
	}
}

/* ---- Minimal RPMh VRM client: shim port of xnu 0097-sweet-rpmh ----
 * Same constants and preservation semantics as the kernel client: only an
 * already-idle ACTIVE (DRV2) TCS is used, inherited messages are never
 * overwritten, sleep/wake banks untouched. MMU is off so ml_io_map
 * collapses to the physical address; delay(1) becomes delay_us(1).
 * Guard relaxed to exactly our rail (pm6150 ldoa18, 1800 mV) instead of
 * the kernel's stale ldoc11-only check that made l18 fail with EINVAL.
 */
#define EINVAL	22
#define ENODEV	19
#define EBUSY	16
#define EIO	5
#define ETIMEDOUT 110

#define RH_RSC_PA	0x18220000u
#define RH_TCS_PA	0x18220d00u
#define RH_TCS_SIZE	0x3000u
#define RH_DICT_PA	0x0c3f000cu
#define RH_DRV		2u
#define RH_ACTIVE_N	2u		/* DT order: ACTIVE 2, SLEEP 3, WAKE 3 */
#define RH_NORMAL_N	8u
#define RH_TCS_STRIDE	0x2a0u
#define RH_CMD_STRIDE	0x14u
#define RH_IRQ_EN	0x00u
#define RH_IRQ_STATUS	0x04u
#define RH_IRQ_CLEAR	0x08u
#define RH_WAIT		0x10u
#define RH_CONTROL	0x14u
#define RH_STATUS	0x18u
#define RH_ENABLE	0x1cu
#define RH_MSGID	0x30u
#define RH_ADDR		0x34u
#define RH_DATA		0x38u
#define RH_CMD_STATUS	0x3cu
#define RH_AMC_EN	(1u << 16)
#define RH_TRIGGER	(1u << 24)
#define RH_ISSUED	(1u << 8)
#define RH_COMPLETE	(1u << 16)
#define RH_WRITE_ACK	(8u | (1u << 8) | (1u << 16))
#define RH_DB_MAGIC	0x0c0330dbu
#define RH_DB_HEADER	144u
#define RH_DB_ENTRY	24u
#define RH_DB_MAX	0x100000u
#define RH_VRM		4u
#define RH_TIMEOUT_US	100000u

static volatile uint32_t *rh_rsc, *rh_tcs, *rh_dict, *rh_db;
static uint32_t rh_db_size;
static unsigned rh_ncpt;
static int rh_poisoned;

static uint32_t
rh_read(volatile uint32_t *base, uint32_t off)
{
	uint32_t v = base[off / 4u];
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
rh_write(volatile uint32_t *base, uint32_t off, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	base[off / 4u] = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

static int
rh_sync(uint32_t off, uint32_t value)
{
	rh_write(rh_tcs, off, value);
	for (unsigned i = 0; i < 1000; ++i) {
		if (rh_read(rh_tcs, off) == value)
			return 1;
		delay_us(1);
	}
	return 0;
}

/* DB is little endian; never issue an unaligned Device load. */
static uint16_t
rh_db16(uint32_t off)
{
	uint32_t v = rh_read(rh_db, off & ~3u);
	return (uint16_t)(v >> (8u * (off & 3u)));
}

static int
rh_range(uint32_t off, uint32_t size)
{
	return off <= rh_db_size && size <= rh_db_size - off;
}

static int
rh_map(void)
{
	rh_rsc = (volatile uint32_t *)RH_RSC_PA;
	rh_tcs = (volatile uint32_t *)RH_TCS_PA;
	rh_dict = (volatile uint32_t *)RH_DICT_PA;
	uint32_t id = rh_read(rh_rsc, 0);
	uint32_t solver = rh_read(rh_rsc, 4);
	uint32_t config = rh_read(rh_rsc, 0x0c);
	unsigned major = (id >> 16) & 0xffu;
	unsigned count = (config >> (6u * RH_DRV)) & 0x3fu;
	rh_ncpt = (config >> 27) & 0x1fu;
	con_hex2("RHid", "id=", id, "slv=", solver);
	con_hex2("RHcf", "cfg=", config, "n=", ((uint32_t)count << 8) | rh_ncpt);
	if ((major != 1 && major != 2) || config == 0xffffffffu ||
	    solver == 0xffffffffu || count < RH_NORMAL_N ||
	    count > RH_TCS_SIZE / RH_TCS_STRIDE || rh_ncpt < 3 || rh_ncpt > 16)
		return EINVAL;
	if (!rh_db) {
		uint32_t pa = rh_read(rh_dict, 0);
		uint32_t size = rh_read(rh_dict, 4);
		con_hex2("RHdb", "pa=", pa, "sz=", size);
		/* Firmware supplies the reserved RAM address; never scan RAM. */
		if (pa < 0x80000000u || (pa & 3u) || (size & 3u) ||
		    size < RH_DB_HEADER || size > RH_DB_MAX)
			return EINVAL;
		rh_db = (volatile uint32_t *)pa;
		rh_db_size = size;
	}
	uint32_t db_version = rh_read(rh_db, 0);
	uint32_t magic = rh_read(rh_db, 4);
	uint32_t flags = rh_read(rh_db, 140);
	con_hex2("RHmg", "ver=", db_version, "mag=", magic);
	if (magic != RH_DB_MAGIC || db_version != 1 || (flags & 1u))
		return EINVAL;
	return 0;
}

static int
rh_lookup(const char *res, uint32_t *address)
{
	uint32_t key[2] = {0, 0};
	for (unsigned i = 0; i < 8 && res[i]; ++i)
		key[i / 4] |= (uint32_t)(uint8_t)res[i] << (8u * (i % 4));
	unsigned matches = 0;
	for (unsigned i = 0; i < 8; ++i) {
		uint32_t h = 8u + 16u * i;
		unsigned slave = rh_db16(h);
		if (!slave)
			break;
		uint32_t entries = RH_DB_HEADER + rh_db16(h + 2);
		uint32_t aux = RH_DB_HEADER + rh_db16(h + 4);
		unsigned count = rh_db16(h + 6);
		if (slave < 3 || slave > 5 || (entries & 3u) ||
		    !rh_range(entries, count * RH_DB_ENTRY) || !rh_range(aux, 0))
			return EINVAL;
		for (unsigned j = 0; j < count; ++j) {
			uint32_t e = entries + RH_DB_ENTRY * j;
			if (rh_read(rh_db, e) != key[0] || rh_read(rh_db, e + 4) != key[1])
				continue;
			uint32_t addr = rh_read(rh_db, e + 16);
			if (slave != RH_VRM || (addr >> 16) != RH_VRM || (addr & 15u) ||
			    !rh_range(aux + rh_db16(e + 22), rh_db16(e + 20)))
				return EINVAL;
			*address = addr;
			++matches;
		}
	}
	if (matches != 1)
		return ENODEV;
	return 0;
}

static int
rh_free(unsigned t)
{
	uint32_t b = t * RH_TCS_STRIDE, s = rh_read(rh_tcs, b + RH_STATUS);
	return s != 0 && s != 0xffffffffu &&
	    rh_read(rh_tcs, b + RH_CONTROL) == 0 &&
	    rh_read(rh_tcs, b + RH_ENABLE) == 0 &&
	    rh_read(rh_tcs, b + RH_WAIT) == 0 &&
	    !(rh_read(rh_tcs, RH_IRQ_STATUS) & (1u << t));
}

static int
rh_send(uint32_t address, unsigned mv)
{
	int chosen = -1;
	for (unsigned t = 0; t < RH_ACTIVE_N; ++t) {
		uint32_t b = t * RH_TCS_STRIDE;
		uint32_t enabled = rh_read(rh_tcs, b + RH_ENABLE);
		if (enabled & ~((1u << rh_ncpt) - 1u))
			return EIO;
		for (unsigned c = 0; c < rh_ncpt; ++c) {
			if ((enabled & (1u << c)) &&
			    (rh_read(rh_tcs, b + RH_ADDR + c * RH_CMD_STRIDE) & ~15u) == address)
				return EBUSY;	/* inherited request for this rail */
		}
		if (chosen < 0 && rh_free(t))
			chosen = (int)t;
	}
	if (chosen < 0)
		return EBUSY;
	unsigned t = (unsigned)chosen;
	uint32_t b = t * RH_TCS_STRIDE, bit = 1u << t;
	uint32_t irq_en = rh_read(rh_tcs, RH_IRQ_EN);
	if (irq_en == 0xffffffffu || !rh_free(t))
		return EBUSY;
	/* Only our TCS's source is masked; other sources/CPU IRQs stay intact. */
	if (!rh_sync(RH_IRQ_EN, irq_en & ~bit))
		return EIO;
	rh_poisoned = 1;	/* any uncertain write below quarantines this slot */
	/* HPM (raw 7) is supported by PMIC5 LDOs; enable comes last. */
	const uint32_t offsets[3] = {0, 8, 4};
	const uint32_t values[3] = {mv, 7, 1};
	for (unsigned c = 0; c < 3; ++c) {
		uint32_t cmd = b + c * RH_CMD_STRIDE;
		if (!rh_sync(cmd + RH_MSGID, RH_WRITE_ACK) ||
		    !rh_sync(cmd + RH_ADDR, address + offsets[c]) ||
		    !rh_sync(cmd + RH_DATA, values[c]))
			return EIO;
	}
	if (!rh_sync(b + RH_WAIT, 7) || !rh_sync(b + RH_ENABLE, 7) ||
	    !rh_sync(b + RH_CONTROL, 0) || !rh_sync(b + RH_CONTROL, RH_AMC_EN))
		return EIO;
	rh_write(rh_tcs, b + RH_CONTROL, RH_AMC_EN | RH_TRIGGER);
	uint32_t irq = 0, idle = 0, status[3] = {0, 0, 0};
	int done = 0, progress = 0;
	for (unsigned us = 0; us < RH_TIMEOUT_US && !done; ++us) {
		irq = rh_read(rh_tcs, RH_IRQ_STATUS);
		idle = rh_read(rh_tcs, b + RH_STATUS);
		int complete = 1;
		for (unsigned c = 0; c < 3; ++c) {
			status[c] = rh_read(rh_tcs, b + RH_CMD_STATUS + c * RH_CMD_STRIDE);
			complete &= status[c] != 0xffffffffu &&
			    (status[c] & (RH_ISSUED | RH_COMPLETE)) == (RH_ISSUED | RH_COMPLETE);
		}
		progress |= idle == 0 || !complete;
		if (idle && idle != 0xffffffffu && irq != 0xffffffffu &&
		    ((irq & bit) || progress) && complete)
			done = 1;
		delay_us(1);
	}
	con_hex2("RHtc", "irq=", irq, "idl=", idle);
	if (!done)
		return ETIMEDOUT;
	/* Proven completion: clear trigger, then free only our commands. */
	if (!rh_sync(b + RH_CONTROL, RH_AMC_EN) || !rh_sync(b + RH_CONTROL, 0) ||
	    !rh_sync(b + RH_ENABLE, 0) || !rh_sync(b + RH_WAIT, 0))
		return EIO;
	rh_write(rh_tcs, RH_IRQ_CLEAR, bit);	/* W1C: never touch other bits */
	if ((rh_read(rh_tcs, RH_IRQ_STATUS) & bit) ||
	    !rh_sync(RH_IRQ_EN, (rh_read(rh_tcs, RH_IRQ_EN) & ~bit) | (irq_en & bit)))
		return EIO;
	rh_poisoned = 0;
	return 0;
}

/* Known safe rails only: touch VDDIO 1.8 V and the LCDB positive bias
 * (touch_lab / AVDD, 5.5 V - shared with the panel). Client logic refutes
 * every other name. */
static int
rpmh_on(const char *res, unsigned mv)
{
	uint32_t address = 0;
	int error = rh_map();

	/* After the map, the guard key lines move on: helper prints only the
	 * lookup/send result; the raw map line (RHid/RHcf/RHdb/RHmg) showed
	 * once in v5 and the operator only tracks the return codes here. */
	if (error)
		return error;
	error = rh_lookup(res, &address);
	con_hex2("RHlk", "rc=", (uint32_t)error, "adr=", address);
	if (!error)
		error = rh_send(address, mv);
	con_hex2("RHse", "rc=", (uint32_t)error, "adr=", address);
	return error;
}

/* Scan the cmd-db VRM slave and list every resource whose name starts
 * with 'l', 'b', 'v' or 't' (ldo*, lab/ibb/b*, vph*, touch...). The scan
 * is what replaces guessing names like lcdb_ldo: the firmware DB is
 * authoritative. 6 entries per line, name+address, screens fit 90 cols. */
static void
rh_db_list(void)
{
	char pbuf2[96];
	char *w = pbuf2;
	unsigned shown = 0, total = 0;

	con_str("DB", "VRM cmd-db resource scan (l/b/v/t*)");
	for (unsigned i = 0; i < 8; ++i) {
		uint32_t h = 8u + 16u * i;
		unsigned slave = rh_db16(h);
		if (!slave) {
			break;
		}
		if (slave != RH_VRM) {
			continue;
		}
		uint32_t entries = RH_DB_HEADER + rh_db16(h + 2);
		unsigned count = rh_db16(h + 6);
		total += count;
		for (unsigned j = 0; j < count; ++j) {
			uint32_t e = entries + RH_DB_ENTRY * j;
			uint32_t key0 = rh_read(rh_db, e);
			uint32_t key1 = rh_read(rh_db, e + 4);
			uint32_t addr = rh_read(rh_db, e + 16);
			unsigned c0 = key0 & 0xffu;
			if (c0 != 'l' && c0 != 'b' && c0 != 'v' && c0 != 't') {
				continue;
			}
			char name[9];
			/* key is packed little-endian, name is <= 8 chars */
			for (unsigned k = 0; k < 8; ++k) {
				name[k] = 0;
			}
			for (unsigned k = 0; k < 8; ++k) {
				uint32_t ch = (k < 4) ? (key0 >> (8u * k)) : (key1 >> (8u * (k - 4)));
				name[k] = (char)(ch & 0xffu);
				if (!name[k]) {
					break;
				}
			}
			name[8] = 0;
			if (w + 16 >= pbuf2 + sizeof(pbuf2)) {
				*w = 0;
				con_str("DB", pbuf2);
				w = pbuf2;
			}
			w = scpy(w, name);
			*w++ = '=';
			w = shex(w, addr, 4);
			*w++ = ' ';
			++shown;
		}
	}
	*w = 0;
	con_str("DB", pbuf2);
	con_hex2("DB", "shown=", shown, "total=", total);
}

static void
rh_lookup_show(const char *res, const char *tag)
{
	uint32_t address = 0;
	int error;

	if (!rh_db && rh_map()) {
		con_hex2(tag, "rc=", EINVAL, "adr=", 0);
		return;
	}
	error = rh_lookup(res, &address);
	con_hex2(tag, "rc=", (uint32_t)error, "adr=", address);
}

void
touch_probe(void)
{
	con_clear(RED);
	con_str("TOUCH", "probe v8 tiles + L18 rail 1.8V");
	con_skip();

	con_hex2("TOUCH", "rst=", gpio_level(PIN_RESET), "irq=", gpio_level(PIN_IRQ));
	con_hex2("BIN", "cfg8=", rd32(GPIO_CFG(PIN_RESET)), "cfg9=", rd32(GPIO_CFG(PIN_IRQ)));

	con_hex2("SPI", "53=", gpio_level(53), "54=", gpio_level(54));
	con_hex2("SPI", "55=", gpio_level(55), "56=", gpio_level(56));

	/* Power the touch VDDIO rail (nobody did this yet), then re-sample
	 * the pads: a live IC starts driving MISO/idle levels. */
	con_hex2("RH18", "rc=", (uint32_t)rpmh_on("ldoa18", 1800), "ten=", 1);
	delay_us(600000);			/* rails + chip boot */
	con_hex2("SPI", "53=", gpio_level(53), "54=", gpio_level(54));
	con_hex2("SPI", "55=", gpio_level(55), "56=", gpio_level(56));
	con_hex2("TOUCH", "rst=", gpio_level(PIN_RESET), "irq=", gpio_level(PIN_IRQ));

	/* Authoritative rail data at the very end, so the probes run first. */
	/* Fall through: with the rail live, reset the chip and sweep the
	 * pin orders (FTS then NVT per order); DB info prints at the end. */
	gpio_input(PIN_RESET, 1);
	gpio_input(PIN_IRQ, 3);
	chip_reset();
	unsigned ms = 0;
	while (ms < 500 && gpio_level(PIN_IRQ)) {
		delay_us(1000);
		++ms;
	}
	con_hex2("IRQ", "low_ms=", ms, "lvl=", gpio_level(PIN_IRQ));

	static const struct bus ord[4] = {
		{53, 54, 55, 56},	/* A: QUP group order */
		{54, 53, 55, 56},	/* B: MOSI/MISO swapped */
		{56, 55, 54, 53},	/* C: group reversed */
		{55, 56, 53, 54},	/* D: CLK first */
	};
	unsigned win = 4;
	for (unsigned i = 0; i < 4; ++i) {
		char lb[2];
		lb[0] = "ABCD"[i]; lb[1] = 0;
		if (try_bus(lb, &ord[i]))
			win = i;
		nvt_try(lb, &ord[i]);
	}
	con_hex2("FOUND", "order=", win, "(0-3|4=none)", 0);

	/* Authoritative rail data at the very end: list the cmd-db VRM
	 * resources named ldoAB, b-type, vph-like or touch-like, and
	 * resolve lab/ibb by exact name. */
	rh_db_list();
	rh_lookup_show("lab", "LAB");
	rh_lookup_show("ibb", "IBB");
}

#endif /* TOUCH_PROBE */
