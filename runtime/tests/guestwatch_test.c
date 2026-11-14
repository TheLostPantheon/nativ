/*
 * Page-protection tracking in runtime/gfx/guestwatch.c, using the production POSIX signal handler
 * and a real 4 GiB guest reservation. On Apple Silicon this also proves that 16 KiB pages fault as
 * SIGBUS. Build and run: see README.md in this directory.
 */
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "../gfx/guestwatch.h"
#include "../platform/platform.h"

uint8_t *recomp_mem;

static int checks, fails;
#define CHECK(cond) do { checks++; if (!(cond)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#ifdef __APPLE__
static volatile sig_atomic_t bus_faults;
static struct sigaction platform_bus;
static int bus_probe_installed;
static void bus_probe(int sig, siginfo_t *info, void *context) {
	bus_faults++;
	if (platform_bus.sa_flags & SA_SIGINFO) platform_bus.sa_sigaction(sig, info, context);
	else platform_bus.sa_handler(sig);
}
static int install_bus_probe(void) {
	if (sigaction(SIGBUS, 0, &platform_bus) || !(platform_bus.sa_flags & SA_SIGINFO)) return -1;
	struct sigaction probe = platform_bus;
	probe.sa_sigaction = bus_probe;
	if (sigaction(SIGBUS, &probe, 0)) return -1;
	bus_probe_installed = 1;
	return 0;
}
#endif

static volatile uint32_t read_hit;
static void read_hook(uint32_t addr) { read_hit = addr; }

static int pipe_with(const void *data, size_t len) {
	int fd[2];
	if (pipe(fd)) return -1;
	ssize_t n = write(fd[1], data, len);
	close(fd[1]);
	if (n != (ssize_t)len) { close(fd[0]); return -1; }
	return fd[0];
}

int main(void) {
	recomp_mem = plat_reserve(1ull << 32);
	if (!recomp_mem) { perror("guest reservation"); return 2; }
	CHECK(gwatch_on());
	const int shift = gwatch_page_shift();
	const uint32_t page_size = 1u << shift;
	CHECK(page_size == plat_page_size());
#ifdef __APPLE__
	CHECK(page_size == 16384);
	CHECK(install_bus_probe() == 0);
#endif

	volatile uint8_t *mem = recomp_mem;
	const uint32_t base_page = 8, base = base_page << shift;
	for (uint32_t p = 0; p < 10; p++) mem[base + p * page_size] = 0;
	uint32_t faults, marks, protects;
	gwatch_stats(&faults, &marks, &protects);

	/* An unaligned range spans three 16 KiB pages. A write opens and stamps only its page. */
	const uint32_t range = base + page_size / 2, range_len = 2 * page_size;
	uint32_t since = gwatch_protect(range, range_len), quiet_since = since;
	CHECK(gwatch_unchanged(range, range_len, &quiet_since));
#ifdef __APPLE__
	sig_atomic_t bus_before = bus_faults;
#endif
	mem[base + page_size + 17] = 0x11;
	CHECK(mem[base + page_size + 17] == 0x11);
#ifdef __APPLE__
	CHECK(bus_faults == bus_before + 1);
#endif
	CHECK(gwatch_page_clock(base_page + 1) > since);
	CHECK(gwatch_page_clock(base_page) <= since);
	CHECK(gwatch_page_clock(base_page + 2) <= since);
	CHECK(!gwatch_unchanged(range, range_len, &since));
	CHECK(gwatch_unchanged(base, page_size / 2, &quiet_since));
	gwatch_stats(&faults, &marks, &protects);
	CHECK(faults == 1 && marks == 0 && protects == 3);

	/* Re-protecting an overlapping range closes the opened page without disturbing its neighbour. */
	uint32_t middle_clock = gwatch_page_clock(base_page + 1);
	since = gwatch_protect(base + page_size, 2 * page_size);
#ifdef __APPLE__
	bus_before = bus_faults;
#endif
	mem[base + 2 * page_size + 23] = 0x22;
#ifdef __APPLE__
	CHECK(bus_faults == bus_before + 1);
#endif
	CHECK(gwatch_page_clock(base_page + 2) > since);
	CHECK(gwatch_page_clock(base_page + 1) == middle_clock);
	CHECK(!gwatch_unchanged(base + page_size, 2 * page_size, &since));
	gwatch_stats(&faults, &marks, &protects);
	CHECK(faults == 1 && marks == 0 && protects == 1);

	/* A kernel write cannot fault on the caller's behalf. Bracketing opens both pages and stamps them. */
	const uint32_t io_addr = base + 5 * page_size - 8;
	const char payload[] = "sixteen bytes!!!";
	since = gwatch_protect(io_addr, sizeof payload);
	int fd = pipe_with(payload, sizeof payload);
	errno = 0;
	CHECK(fd >= 0);
	CHECK(read(fd, recomp_mem + io_addr, sizeof payload) == -1 && errno == EFAULT);
	close(fd);
	fd = pipe_with(payload, sizeof payload);
	CHECK(fd >= 0);
#ifdef __APPLE__
	bus_before = bus_faults;
#endif
	gwatch_write_begin(io_addr, sizeof payload);
	CHECK(read(fd, recomp_mem + io_addr, sizeof payload) == (ssize_t)sizeof payload);
	gwatch_write_end(io_addr, sizeof payload);
	close(fd);
	CHECK(!memcmp(recomp_mem + io_addr, payload, sizeof payload));
#ifdef __APPLE__
	CHECK(bus_faults == bus_before);
#endif
	CHECK(!gwatch_unchanged(io_addr, sizeof payload, &since));
	gwatch_stats(&faults, &marks, &protects);
	CHECK(faults == 0 && marks == 1 && protects == 2);

	/* The read watch uses the same fault path, reports the exact byte, and can be opened proactively. */
	const uint32_t read_addr = base + 7 * page_size;
	mem[read_addr + 31] = 0x5a;
	gwatch_set_read_hook(read_hook);
	gwatch_read_arm(read_addr, page_size);
#ifdef __APPLE__
	bus_before = bus_faults;
#endif
	CHECK(mem[read_addr + 31] == 0x5a);
	CHECK(read_hit == read_addr + 31);
#ifdef __APPLE__
	CHECK(bus_faults == bus_before + 1);
#endif
	gwatch_stats(&faults, &marks, &protects);
	CHECK(faults == 1 && marks == 0 && protects == 1);

	read_hit = 0;
	gwatch_read_arm(read_addr, page_size);
#ifdef __APPLE__
	bus_before = bus_faults;
#endif
	gwatch_read_open(read_addr + 31, 1);
	CHECK(mem[read_addr + 31] == 0x5a);
	CHECK(read_hit == 0);
#ifdef __APPLE__
	CHECK(bus_faults == bus_before);
	if (bus_probe_installed) sigaction(SIGBUS, &platform_bus, 0);
#endif

	munmap(recomp_mem, 1ull << 32);
	fprintf(stderr, "%d checks, %d failed (page size %u)\n", checks, fails, page_size);
	return fails ? 1 : 0;
}
