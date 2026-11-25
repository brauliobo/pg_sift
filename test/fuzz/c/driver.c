/*
 * fz_driver.c - a coverage-guided mutation fuzzer for the targets of this directory when libFuzzer is not there (gcc, an old clang):
 * main() for any LLVMFuzzerTestOneInput. The harnesses are built with -fsanitize-coverage=trace-pc; this file is not, and counts
 * the edges the callback reports in a 64K bitmap, keeping every input that reaches a new one. Same command line as libFuzzer, the
 * options that matter:
 *
 *   target [-max_total_time=SEC] [-runs=N] [-seed=N] [-max_len=N] [-timeout=SEC] [-artifact_prefix=P] CORPUS_DIR|FILE...
 *
 * A directory is read (not recursively) as the seed corpus and every kept input is also written to the first directory of the
 * list; a file is run once (replay of a crash). A crash (AddressSanitizer, UBSan with -fno-sanitize-recover, abort, SIGSEGV) or a
 * timeout writes the running input to P-crash-<hash> / P-timeout-<hash> before dying.
 */
#include <dirent.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern int	LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
extern int	LLVMFuzzerInitialize(int *argc, char ***argv) __attribute__((weak));
extern void __sanitizer_set_death_callback(void (*cb) (void)) __attribute__((weak));

#define MAPSIZE (1 << 16)
static uint8_t map[MAPSIZE];
static uint32_t newbits;

void
__sanitizer_cov_trace_pc(void)
{
	uintptr_t	a = (uintptr_t) __builtin_return_address(0);
	uint32_t	h = (uint32_t) ((a * 0x9E3779B97F4A7C15ull) >> 48);

	if (!map[h])
	{
		map[h] = 1;
		newbits++;
	}
}

typedef struct Input
{
	uint8_t    *data;
	size_t		len;
} Input;

static Input *corpus;
static size_t ncorpus,
			capcorpus;
static const uint8_t *cur;
static size_t curlen;
static const char *prefix = "./";
static unsigned timeout_s = 20;
static uint64_t rng = 88172645463325252ull;

static uint64_t
rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}

static uint64_t
fnv(const uint8_t *p, size_t n)
{
	uint64_t	h = 1469598103934665603ull;

	while (n--)
		h = (h ^ *p++) * 1099511628211ull;
	return h;
}

static void
save(const char *dir, const char *kind, const uint8_t *p, size_t n, int announce)
{
	char		path[1024];
	FILE	   *f;

	snprintf(path, sizeof(path), "%s%s%016llx", dir, kind, (unsigned long long) fnv(p, n));
	if ((f = fopen(path, "wb")))
	{
		fwrite(p, 1, n, f);
		fclose(f);
	}
	if (announce)
		fprintf(stderr, "artifact: %s (%zu bytes)\n", path, n);
}

static void
on_death(void)
{
	char		kind[1100];

	snprintf(kind, sizeof(kind), "%scrash-", prefix);
	save("", kind, cur, curlen, 1);
}

static void
on_alarm(int sig)
{
	char		kind[1100];

	(void) sig;
	snprintf(kind, sizeof(kind), "%stimeout-", prefix);
	save("", kind, cur, curlen, 1);
	fprintf(stderr, "ERROR: timeout (%us)\n", timeout_s);
	_exit(70);
}

static void
on_crash(int sig)
{
	on_death();
	signal(sig, SIG_DFL);
	raise(sig);
}

static int
run(const uint8_t *p, size_t n)
{
	cur = p;
	curlen = n;
	alarm(timeout_s);
	LLVMFuzzerTestOneInput(p, n);
	alarm(0);
	return 0;
}

static void
add(const uint8_t *p, size_t n)
{
	if (ncorpus == capcorpus)
		corpus = realloc(corpus, (capcorpus = capcorpus ? capcorpus * 2 : 64) * sizeof(Input));
	corpus[ncorpus].data = malloc(n ? n : 1);
	memcpy(corpus[ncorpus].data, p, n);
	corpus[ncorpus++].len = n;
}

static uint8_t *
slurp(const char *path, size_t *n)
{
	FILE	   *f = fopen(path, "rb");
	uint8_t    *p;
	long		len;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	p = malloc(len > 0 ? len : 1);
	*n = fread(p, 1, len > 0 ? len : 0, f);
	fclose(f);
	return p;
}

static const uint32_t interesting[] = {0, 1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 63, 64, 127, 128, 255, 256, 1023, 1024, 4095, 4096, 0x7fff,
									   0x8000, 0xffff, 0x10000, 0xffffff, 0x1000000, 0x7fffffff, 0x80000000u, 0xfffffff0u, 0xffffffffu};

/* one havoc round on buf (cap bytes, *n in use) */
static void
mutate(uint8_t *buf, size_t *n, size_t cap)
{
	for (int rounds = 1 + (int) (rnd() % 4); rounds--;)
	{
		size_t		len = *n,
					i = len ? rnd() % len : 0,
					j;
		Input	   *o = &corpus[rnd() % ncorpus];

		switch (rnd() % 11)
		{
			case 0:
				if (len) buf[i] ^= (uint8_t) (1u << (rnd() % 8));
				break;
			case 1:
				if (len) buf[i] = (uint8_t) rnd();
				break;
			case 2:
				if (len) buf[i] += (uint8_t) (rnd() % 35) - 17;
				break;
			case 3:
				if (len >= 4) { uint32_t v = interesting[rnd() % (sizeof(interesting) / 4)]; memcpy(buf + i % (len - 3), &v, 4); }
				break;
			case 4:
				if (len >= 2) { uint16_t v = (uint16_t) interesting[rnd() % (sizeof(interesting) / 4)]; memcpy(buf + i % (len - 1), &v, 2); }
				break;
			case 5:
				if (len >= 8) { uint64_t v = rnd() % 2 ? ~0ull : (uint64_t) interesting[rnd() % (sizeof(interesting) / 4)]; memcpy(buf + i % (len - 7), &v, 8); }
				break;
			case 6:				/* delete a block */
				if (len > 1) { size_t d = 1 + rnd() % (len / 2 + 1); if (d > len - i) d = len - i; memmove(buf + i, buf + i + d, len - i - d); *n = len - d; }
				break;
			case 7:				/* insert random bytes */
				{
					size_t d = 1 + rnd() % 16;

					if (len + d <= cap && len)
					{
						memmove(buf + i + d, buf + i, len - i);
						for (j = 0; j < d; j++) buf[i + j] = (uint8_t) rnd();
						*n = len + d;
					}
				}
				break;
			case 8:				/* copy a block of itself over another place */
				if (len > 2) { size_t d = 1 + rnd() % 32, s = rnd() % len; if (d > len - s) d = len - s; if (d > len - i) d = len - i; memmove(buf + i, buf + s, d); }
				break;
			case 9:				/* splice a block of another input */
				if (o->len && len)
				{
					size_t s = rnd() % o->len, d = 1 + rnd() % 64;

					if (d > o->len - s) d = o->len - s;
					if (d > len - i) d = len - i;
					memcpy(buf + i, o->data + s, d);
				}
				break;
			default:			/* truncate or extend with zeros */
				if (len > 1 && rnd() % 2) *n = 1 + rnd() % len;
				else if (len + 8 <= cap) { memset(buf + len, 0, 8); *n = len + 8; }
				break;
		}
	}
}

int
main(int argc, char **argv)
{
	long		max_time = 0,
				max_runs = -1;
	size_t		max_len = 1 << 16;
	const char *outdir = NULL;
	struct timespec t0;
	uint8_t    *buf;
	long		runs = 0;

	if (LLVMFuzzerInitialize)
		LLVMFuzzerInitialize(&argc, &argv);
	signal(SIGALRM, on_alarm);
	signal(SIGSEGV, on_crash);
	signal(SIGBUS, on_crash);
	signal(SIGFPE, on_crash);
	signal(SIGILL, on_crash);
	signal(SIGABRT, on_crash);
	if (__sanitizer_set_death_callback)
		__sanitizer_set_death_callback(on_death);
	for (int a = 1; a < argc; a++)
	{
		const char *s = argv[a];
		struct stat st;

		if (!strncmp(s, "-max_total_time=", 16)) max_time = atol(s + 16);
		else if (!strncmp(s, "-runs=", 6)) max_runs = atol(s + 6);
		else if (!strncmp(s, "-seed=", 6)) rng ^= (uint64_t) atol(s + 6) * 0x9E3779B97F4A7C15ull;
		else if (!strncmp(s, "-max_len=", 9)) max_len = (size_t) atol(s + 9);
		else if (!strncmp(s, "-timeout=", 9)) timeout_s = (unsigned) atoi(s + 9);
		else if (!strncmp(s, "-artifact_prefix=", 17)) prefix = s + 17;
		else if (s[0] == '-') continue;			/* libFuzzer options with no meaning here */
		else if (stat(s, &st) == 0 && S_ISDIR(st.st_mode))
		{
			DIR		   *d = opendir(s);
			struct dirent *e;

			if (!outdir) outdir = s;
			while (d && (e = readdir(d)))
			{
				char path[1024];
				size_t n;
				uint8_t *p;

				snprintf(path, sizeof(path), "%s/%s", s, e->d_name);
				if (stat(path, &st) || !S_ISREG(st.st_mode) || !(p = slurp(path, &n)))
					continue;
				add(p, n > max_len ? max_len : n);
				free(p);
			}
			if (d) closedir(d);
		}
		else
		{
			size_t n;
			uint8_t *p = slurp(s, &n);

			if (!p) { fprintf(stderr, "cannot read %s\n", s); return 2; }
			run(p, n);
			fprintf(stderr, "Executed %s\n", s);
			free(p);
			return 0;
		}
	}
	if (!ncorpus)
		add((const uint8_t *) "\0\0\0\0\0\0\0\0", 8);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (size_t i = 0; i < ncorpus; i++)
		run(corpus[i].data, corpus[i].len), runs++;
	fprintf(stderr, "INITED corpus %zu, edges %u\n", ncorpus, newbits);
	buf = malloc(max_len + 64);
	while (max_runs < 0 || runs < max_runs)
	{
		struct timespec t1;
		Input	   *b = &corpus[rnd() % ncorpus];
		size_t		n = b->len > max_len ? max_len : b->len;
		uint32_t	before = newbits;

		if (max_time)
		{
			clock_gettime(CLOCK_MONOTONIC, &t1);
			if (t1.tv_sec - t0.tv_sec >= max_time) break;
		}
		else if (max_runs < 0 && runs > 1000000)
			break;
		memcpy(buf, b->data, n);
		mutate(buf, &n, max_len);
		run(buf, n);
		runs++;
		if (newbits != before)
		{
			add(buf, n);
			if (outdir) save(outdir, "/", buf, n, 0);
			if (ncorpus % 50 == 0)
				fprintf(stderr, "#%ld\tNEW\tcorpus %zu\tedges %u\n", runs, ncorpus, newbits);
		}
	}
	fprintf(stderr, "Done %ld runs, corpus %zu, edges %u\n", runs, ncorpus, newbits);
	return 0;
}
