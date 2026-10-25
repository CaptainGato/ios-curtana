/*
 * Curtana/miatoll touch-bus probe, compiled into the shim only when
 * TOUCH_PROBE=1. Runs with the MMU off (Device memory), no policy:
 *
 *   - reports the TLMM levels of reset/irq and of the four QUP1-SE pins
 *   - pulses the touch reset line and watches the IRQ for 500 ms
 *   - bit-bangs the FocalTech SPI frame format (focaltech_ts: 4-byte
 *     header + 3 dummy bytes + data [+ 2 CRC bytes]) on the pin group
 *     the live FDT assigns to spi@a94000 (gpio 53..56, function qup15)
 *   - prints every attempt, including the raw status byte, so a photo of
 *     the screen tells us pin order, rail state and chip id in one boot
 *
 * The shim then waits 45 s and resets to Android. Nothing is written.
 */
#include <stdint.h>
#include "console.h"

#if defined(TOUCH_PROBE) && TOUCH_PROBE

#define TLMM		0x03400000UL
#define GPIO_CFG(n)	(TLMM + (uintptr_t)(n) * 0x1000UL)
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

/* Read the boot id (0x55 0xaa handshake, then reg 0x90) and the app ids. */
static void
try_bus(const char *tag, const struct bus *b)
{
	uint8_t id[2] = {0}, v[2] = {0}, rawhdr[4] = {0};
	int crc = 0;
	uint8_t st = 0;

	bus_init(b);
	chip_reset();
	delay_us(20000);

	const uint8_t start[2] = {0x55, 0xaa};
	int w = fts_write(b, 0x55, &start[1], 1);
	delay_us(15000);
	int r = fts_read(b, 0x90, id, 2, &crc, &st);

	con_hex2(tag, "wr=", w, "rd=", r);
	con_hex2(tag, "st=", st, "id=", (id[0] << 8) | id[1]);
	con_hex2(tag, "crc=", crc, "irq=", gpio_level(PIN_IRQ));

	/* A single 0xa3 read is enough for a valid app-mode IC. */
	crc = 0;
	st = 0;
	if (!fts_read(b, 0xa3, v, 1, &crc, &st)) {
		con_hex2(tag, "a3=", v[0], "crc=", crc);
	}
	crc = 0;
	st = 0;
	if (!fts_read(b, 0x9f, v, 1, &crc, &st)) {
		con_hex2(tag, "9f=", v[0], "crc=", crc);
	}
	(void)rawhdr;
}

void
touch_probe(void)
{
	con_clear(RED);
	con_str("TOUCH", "curtana SPI probe (TLMM 0x3400000)");
	con_skip();
	con_hex2("TOUCH", "rst=", gpio_level(PIN_RESET), "irq=", gpio_level(PIN_IRQ));
	con_hex2("TOUCH", "53=", gpio_level(53), "54=", gpio_level(54));
	con_hex2("TOUCH", "55=", gpio_level(55), "56=", gpio_level(56));
	con_skip();

	/* Alive check that needs no SPI: FTS asserts IRQ (low) after reset. */
	gpio_input(PIN_RESET, 3);
	gpio_input(PIN_IRQ, 3);
	chip_reset();
	unsigned ms = 0;
	while (ms < 500 && gpio_level(PIN_IRQ)) {
		delay_us(1000);
		++ms;
	}
	con_hex2("TOUCH IRQ", "low_after_ms=", ms, "lvl=", gpio_level(PIN_IRQ));
	con_skip();

	/* Pin order A: qcom QUP group order (MOSI, MISO, CLK, CS). */
	struct bus a = {53, 54, 55, 56};
	try_bus("TOUCH A", &a);
	/* Pin order B: swap MOSI/MISO. */
	struct bus bb = {54, 53, 55, 56};
	try_bus("TOUCH B", &bb);
	/* Pin order C: group reversed (CS, CLK, MISO, MOSI). */
	struct bus c = {56, 55, 54, 53};
	try_bus("TOUCH C", &c);
	/* Pin order D: CLK first. */
	struct bus d = {55, 56, 53, 54};
	try_bus("TOUCH D", &d);

	con_skip();
	con_str("TOUCH", "done, resetting in 45 s");
}

#endif /* TOUCH_PROBE */
