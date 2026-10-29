// SPDX-License-Identifier: GPL-2.0-only
/*
 * Deterministic mutation runner (design 10.6) over the same walk as the
 * libFuzzer harness.
 *
 *   mutate_runner --pack RAW.img OUT.wsi        walk a raw image, store the sectors it reads
 *   mutate_runner --unmutated BASE.wsi          one clean walk, must see no error at all
 *   mutate_runner --corpus DIR                  run every *.wsd (bad-*: must report an error)
 *                                                and every base-*.wsi (unmutated)
 *   mutate_runner --sample PCT [--seed N] [--fork] [--jobs J] [--dump-dir D] BASE.wsi
 *                                               PCT percent of all mutations (100 = full run);
 *                                               --jobs splits them over J processes
 *   mutate_runner --only INDEX BASE.wsi         replay one mutation (the index in a report)
 *
 * Mutations per sector of the base (the sectors the unmutated walk reads),
 * 1090 each: [0] all 0x00, [1] all 0xFF, [2..1025] flip one bit of the first
 * 128 bytes, [1026..1089] seeded random byte overwrites (64 variants).
 * Which indices a PCT run takes depends only on (seed, index): the same
 * selection on every machine. --fork runs the mutations in a child process,
 * logs a crash/hang as FAIL with the index, and continues with the next one.
 * --dump-dir writes every failing mutation as a .wsd for the corpus.
 */
#include "fuzz_walk.h"
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

/* The backend's own allocations are checked by the shim's live-byte count
 * after every run; LSan would only flag the runner's loaded images. */
const char *__asan_default_options(void) { return "detect_leaks=0:abort_on_error=0"; }

#define PER_SECTOR	1090u
#define RUN_LIMIT_S	10.0
#define HANG_ALARM_S	60

static uint64_t mix(uint64_t z)
{
	z += 0x9e3779b97f4a7c15ull;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}

/* Build mutation `idx` of the base into *o. */
static void make_mutation(const struct fz_img *im, uint64_t seed, uint64_t idx, struct fz_ovl *o)
{
	const struct fz_sec *s = &im->sec[idx / PER_SECTOR];
	unsigned k = (unsigned)(idx % PER_SECTOR);

	o->role = s->role;
	o->no = s->no;
	memcpy(o->d, s->d, FZ_SEC);
	if (k == 0) {
		memset(o->d, 0x00, FZ_SEC);
	} else if (k == 1) {
		memset(o->d, 0xFF, FZ_SEC);
	} else if (k < 1026) {
		o->d[(k - 2) / 8] ^= (uint8_t)(1u << ((k - 2) % 8));
	} else {
		uint64_t r = mix(seed ^ mix(idx));
		unsigned cnt = 1 + (unsigned)(r & 15), i;

		for (i = 0; i < cnt; i++) {
			r = mix(r);
			unsigned pos = (r & 1) ? (unsigned)((r >> 8) % 128) : (unsigned)((r >> 8) % FZ_SEC);

			o->d[pos] = (uint8_t)(r >> 32);
		}
	}
}

static void describe(const struct fz_img *im, uint64_t idx, char *out, size_t n)
{
	const struct fz_sec *s = &im->sec[idx / PER_SECTOR];
	unsigned k = (unsigned)(idx % PER_SECTOR);

	snprintf(out, n, "idx %llu (dev %u sector %llu, %s %u)", (unsigned long long)idx, s->role,
		 (unsigned long long)s->no,
		 k == 0 ? "zero" : k == 1 ? "ones" : k < 1026 ? "bitflip" : "random",
		 k < 2 ? 0 : k < 1026 ? k - 2 : k - 1026);
}

static int selected(uint64_t seed, uint64_t idx, double pct)
{
	if (pct >= 100.0)
		return 1;
	return (double)(mix(seed ^ (idx * 0xd1342543de82ef95ull)) >> 11) / 9007199254740992.0 < pct / 100.0;
}

/* ---- commands ---- */

static int cmd_pack(const char *raw, const char *out)
{
	struct fz_img im = { .raw_fd = open(raw, O_RDONLY) };
	struct stat st;
	struct fz_res r;
	size_t i, m = 0;

	if (im.raw_fd < 0 || fstat(im.raw_fd, &st))
		return perror(raw), 2;
	im.size[0] = (uint64_t)st.st_size;
	fz_run(&im, NULL, 0, 0, &r);
	if (r.mount_err || r.nerr || fz_fail(&r)) {
		fprintf(stderr, "%s: the clean walk is not clean (mount %d, errors %u, %s)\n", raw,
			r.mount_err, r.nerr, r.first);
		return 1;
	}
	qsort(fz.rec, fz.nrec, sizeof(*fz.rec), fz_cmp_sec);
	for (i = 0; i < fz.nrec; i++)
		if (i == 0 || fz_cmp_sec(&fz.rec[i], &fz.rec[i - 1]))
			fz.rec[m++] = fz.rec[i];
	im.sec = fz.rec;
	im.n = m;
	im.raw_fd = -1;
	if (fz_save_wsi(&im, out))
		return perror(out), 2;
	printf("packed %zu sectors (%u reads) -> %s\n", m, (unsigned)r.nreads, out);
	return 0;
}

static void print_res(const char *what, const struct fz_res *r)
{
	printf("%s: mount=%d errors=%u ops=%u reads=%lu%s%s\n", what, r->mount_err, r->nerr, r->nops,
	       r->nreads, fz_fail(r) ? " FAIL: " : "", fz_fail(r) ? r->first : "");
}

static int cmd_unmutated(const char *path)
{
	struct fz_img im;
	struct fz_res r;
	int bad;

	if (fz_load_wsi(&im, path))
		return perror(path), 2;
	fz_run(&im, NULL, 0, RUN_LIMIT_S, &r);
	bad = r.mount_err || r.nerr || fz_fail(&r) || r.nops < 20;

	print_res("unmutated_walk_ok", &r);
	printf("unmutated_walk_ok: %s\n", bad ? "FAIL" : "ok");
	return bad;
}

static int has_suffix(const char *s, const char *suf)
{
	size_t a = strlen(s), b = strlen(suf);

	return a >= b && !strcmp(s + a - b, suf);
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int cmd_corpus(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	char **names = NULL;
	size_t n = 0, i;
	int rc = 0;

	if (!d)
		return perror(dir), 2;
	while ((e = readdir(d))) {
		if (has_suffix(e->d_name, ".wsd") || has_suffix(e->d_name, ".wsi")) {
			names = realloc(names, (n + 1) * sizeof(*names));
			names[n++] = strdup(e->d_name);
		}
	}
	closedir(d);
	qsort(names, n, sizeof(*names), cmp_str);
	for (i = 0; i < n; i++) {
		char path[1024];
		struct fz_img im;
		struct fz_ovl ovl[FZ_MAX_OVL];
		struct fz_res r;
		int no = 0, wsd = has_suffix(names[i], ".wsd");
		int want_err = !strncmp(names[i], "bad-", 4);
		int ok;

		snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
		if (wsd) {
			no = fz_load_wsd(&im, ovl, FZ_MAX_OVL, path);
			if (no < 0) {
				fprintf(stderr, "%s: unreadable\n", path);
				rc = 1;
				continue;
			}
		} else if (fz_load_wsi(&im, path)) {
			fprintf(stderr, "%s: unreadable\n", path);
			rc = 1;
			continue;
		}
		fz_run(&im, ovl, no, RUN_LIMIT_S, &r);
		/* bad-*: clean error codes, at least one. reg-*: only no failure. base-*: clean. */
		ok = !fz_fail(&r) && (want_err ? r.nerr > 0 : (wsd || !r.nerr));
		printf("%-4s %s: mount=%d errors=%u reads=%lu%s%s\n", ok ? "ok" : "FAIL", names[i],
		       r.mount_err, r.nerr, r.nreads, r.first[0] ? "  " : "", r.first);
		rc |= !ok;
		free(im.sec);
		free(names[i]);
	}
	free(names);
	printf("known_bad_inputs_return_errors: %s (%zu inputs)\n", rc ? "FAIL" : "ok", n);
	return rc;
}

/* shared with the child: index being run */
static volatile uint64_t *g_cur;
static const struct fz_img *g_im;

static void on_alarm(int sig)
{
	char b[160];
	int n;

	(void)sig;
	describe(g_im, *g_cur, b, sizeof(b));
	n = (int)strlen(b);
	fprintf(stderr, "HANG (alarm %ds): %.*s\n", HANG_ALARM_S, n, b);
	_exit(86);
}

static const char *g_dump;
static const char *g_basename;

static void dump_failure(const struct fz_img *im, uint64_t seed, uint64_t idx)
{
	struct fz_ovl o;
	char p[1024];

	if (!g_dump)
		return;
	make_mutation(im, seed, idx, &o);
	snprintf(p, sizeof(p), "%s/fail-%llu.wsd", g_dump, (unsigned long long)idx);
	fz_save_wsd(p, g_basename, &o, 1);
	fprintf(stderr, "  saved %s\n", p);
}

static int run_one(const struct fz_img *im, uint64_t seed, uint64_t idx, int verbose, volatile uint64_t *stat)
{
	struct fz_ovl o;
	struct fz_res r;
	char b[160];

	make_mutation(im, seed, idx, &o);
	fz_run(im, &o, 1, RUN_LIMIT_S, &r);
	if (stat)
		stat[r.mount_err ? 0 : r.nerr ? 1 : 2]++;
	if (fz_fail(&r) || verbose) {
		describe(im, idx, b, sizeof(b));
		printf("%s %s: mount=%d errors=%u reads=%lu %s\n", fz_fail(&r) ? "FAIL" : "ok", b,
		       r.mount_err, r.nerr, r.nreads, r.first);
	}
	return fz_fail(&r);
}

static unsigned g_shard, g_nshards = 1;

static int cmd_sample(const char *path, double pct, uint64_t seed, int dofork, int only, uint64_t only_idx)
{
	struct fz_img im;
	uint64_t total, idx, ran, failed = 0, crashed = 0;
	volatile uint64_t *cur;
	double t0 = fz_now();

	if (fz_load_wsi(&im, path) || im.n == 0)
		return perror(path), 2;
	g_im = &im;
	total = im.n * PER_SECTOR;
	if (only) {
		if (only_idx >= total)
			return fprintf(stderr, "index beyond %llu\n", (unsigned long long)total), 2;
		return run_one(&im, seed, only_idx, 1, NULL);
	}
	cur = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	g_cur = cur;
	if (g_shard == 0)
		printf("base %s: %zu sectors, %llu mutations, sample %.3g%% seed %llu%s\n", path, im.n,
	       (unsigned long long)total, pct, (unsigned long long)seed, dofork ? " (forked)" : "");
	fflush(stdout);
	idx = 0;
	while (idx < total) {
		pid_t pid;
		int status = 0;

		fflush(stdout);
		pid = dofork ? fork() : 0;
		if (pid < 0)
			return perror("fork"), 2;
		if (pid == 0) {
			struct sigaction sa = { .sa_handler = on_alarm };

			sigaction(SIGALRM, &sa, NULL);
			for (; idx < total; idx++) {
				if (idx % g_nshards != g_shard || !selected(seed, idx, pct))
					continue;
				*cur = idx;
				alarm(HANG_ALARM_S);
				if (run_one(&im, seed, idx, 0, cur + 3)) {
					dump_failure(&im, seed, idx);
					cur[2]++;
				}
				cur[1]++;
			}
			alarm(0);
			if (dofork)
				_exit(0);
			break;
		}
		waitpid(pid, &status, 0);
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
			break;
		{
			char b[160];
			uint64_t bad = *cur;

			describe(&im, bad, b, sizeof(b));
			printf("FAIL %s: %s\n", b,
			       WIFSIGNALED(status) ? "killed by signal (sanitizer abort/crash)" : "exit code (sanitizer report or hang)");
			dump_failure(&im, seed, bad);
			crashed++;
			idx = bad + 1;
		}
	}
	ran = cur[1] + crashed;
	failed = cur[2] + crashed;
	printf("shard %u/%u: %llu mutations run (mount refused %llu, walk errors %llu, clean %llu), "
	       "failures %llu (crash/hang %llu), %.1fs\n", g_shard, g_nshards, (unsigned long long)ran,
	       (unsigned long long)cur[3], (unsigned long long)cur[4], (unsigned long long)cur[5],
	       (unsigned long long)failed, (unsigned long long)crashed, fz_now() - t0);
	fflush(stdout);
	return failed != 0;
}

int main(int argc, char **argv)
{
	double pct = 1.0;
	uint64_t seed = 0x58465321, only_idx = 0;
	int i, dofork = 0, only = 0, mode = 0, jobs = 1;
	const char *arg1 = NULL, *arg2 = NULL, *base = NULL;

	if (getenv("RD_VERBOSE"))
		fz.verbose = 1;
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--pack") && i + 2 < argc) {
			mode = 1; arg1 = argv[++i]; arg2 = argv[++i];
		} else if (!strcmp(argv[i], "--unmutated") && i + 1 < argc) {
			mode = 2; arg1 = argv[++i];
		} else if (!strcmp(argv[i], "--corpus") && i + 1 < argc) {
			mode = 3; arg1 = argv[++i];
		} else if (!strcmp(argv[i], "--sample") && i + 1 < argc) {
			mode = 4; pct = atof(argv[++i]);
		} else if (!strcmp(argv[i], "--only") && i + 1 < argc) {
			mode = 4; only = 1; only_idx = strtoull(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
			seed = strtoull(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--jobs") && i + 1 < argc) {
			jobs = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--fork")) {
			dofork = 1;
		} else if (!strcmp(argv[i], "--dump-dir") && i + 1 < argc) {
			g_dump = argv[++i];
		} else if (argv[i][0] != '-' && !base) {
			base = argv[i];
		} else {
			fprintf(stderr, "bad argument: %s (see the header of mutate_runner.c)\n", argv[i]);
			return 2;
		}
	}
	if (base) {
		const char *s = strrchr(base, '/');

		g_basename = s ? s + 1 : base;
	}
	switch (mode) {
	case 1: return cmd_pack(arg1, arg2);
	case 2: return cmd_unmutated(arg1);
	case 3: return cmd_corpus(arg1);
	case 4:
		if (!base)
			return fprintf(stderr, "base .wsi missing\n"), 2;
		if (jobs > 1 && !only) {
			int j, rc = 0;

			fflush(stdout);
			g_nshards = (unsigned)jobs;
			for (j = 0; j < jobs; j++) {
				if (fork() == 0) {
					g_shard = (unsigned)j;
					_exit(cmd_sample(base, pct, seed, dofork, 0, 0));
				}
			}
			for (j = 0; j < jobs; j++) {
				int st;

				wait(&st);
				rc |= !WIFEXITED(st) || WEXITSTATUS(st);
			}
			return rc;
		}
		return cmd_sample(base, pct, seed, dofork, only, only_idx);
	}
	fprintf(stderr, "usage: see the header of mutate_runner.c\n");
	return 2;
}
