// SPDX-License-Identifier: GPL-2.0-only
/* Standalone tests of the kernel compatibility shim base. */
#include "wfs_shim.h"
#include <stdio.h>
#include <string.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

static u32 crc_std(const void *p, size_t n)
{
	return ~crc32c_le(~0u, p, n);
}

static void crc32c_known_vectors(void)
{
	u8 b[32];
	int i;

	memset(b, 0, 32);
	CHECK(crc_std(b, 32) == 0x8A9136AAu);
	memset(b, 0xFF, 32);
	CHECK(crc_std(b, 32) == 0x62A8AB43u);
	for (i = 0; i < 32; i++) b[i] = (u8)i;
	CHECK(crc_std(b, 32) == 0x46DD794Eu);
	for (i = 0; i < 32; i++) b[i] = (u8)(31 - i);
	CHECK(crc_std(b, 32) == 0x113FDB5Cu);
	CHECK(crc_std("123456789", 9) == 0xE3069283u);
	CHECK(crc_std("", 0) == 0);
}

static void crc32c_chunking_and_alignment(void)
{
	u8 buf[300];
	size_t i, split;
	u32 whole;

	for (i = 0; i < sizeof(buf); i++) buf[i] = (u8)(i * 7 + 3);
	for (i = 0; i < 9; i++) {	/* unaligned starts */
		whole = crc32c_le(~0u, buf + i, 250);
		for (split = 0; split <= 250; split += 17)
			CHECK(crc32c_le(crc32c_le(~0u, buf + i, split),
					buf + i + split, 250 - split) == whole);
	}
}

static void errno_values_are_linux(void)
{
	CHECK(EPERM == 1 && ENOENT == 2 && EIO == 5 && ENOMEM == 12);
	CHECK(ENODEV == 19 && ENOTDIR == 20 && EINVAL == 22 && EFBIG == 27);
	CHECK(ENOSPC == 28 && EROFS == 30 && ENAMETOOLONG == 36);
	CHECK(ENOSYS == 38 && ENOTEMPTY == 39 && ELOOP == 40);
	CHECK(ENODATA == 61 && EBADMSG == 74 && EOVERFLOW == 75);
	CHECK(EOPNOTSUPP == 95 && ESTALE == 116 && EUCLEAN == 117);
	CHECK(EFSCORRUPTED == EUCLEAN && EFSBADCRC == EBADMSG);
	CHECK(ENOATTR == ENODATA);
	CHECK(EWRONGFS == EINVAL);
	CHECK(EFSCORRUPTED == WFS_EUCLEAN && EFSBADCRC == WFS_EBADMSG);
}

static void endian_roundtrip_be16_32_64(void)
{
	u8 raw[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
	__be16 a; __be32 b; __be64 c;

	memcpy(&a, raw, 2); memcpy(&b, raw, 4); memcpy(&c, raw, 8);
	CHECK(be16_to_cpu(a) == 0x0102);
	CHECK(be32_to_cpu(b) == 0x01020304u);
	CHECK(be64_to_cpu(c) == 0x0102030405060708ull);
	CHECK(cpu_to_be16(0x0102) == a);
	CHECK(cpu_to_be32(0x01020304u) == b);
	CHECK(cpu_to_be64(0x0102030405060708ull) == c);
	CHECK(le16_to_cpu(cpu_to_le16(0xBEEF)) == 0xBEEF);
	CHECK(le32_to_cpu(cpu_to_le32(0xDEADBEEFu)) == 0xDEADBEEFu);
	CHECK(le64_to_cpu(cpu_to_le64(0x1122334455667788ull)) == 0x1122334455667788ull);
	{ u8 le[4]; u32 v = cpu_to_le32(0x01020304u); memcpy(le, &v, 4);
	  CHECK(le[0] == 4 && le[3] == 1); }
	{ u8 m[9] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
	  CHECK(get_unaligned_be32(m + 1) == 0x01020304u);
	  CHECK(get_unaligned_be64(m + 1) == 0x0102030405060708ull);
	  put_unaligned_be32(0xAABBCCDDu, m + 3);
	  CHECK(m[3] == 0xAA && m[6] == 0xDD); }
}

static void bitops_are_64bit_safe(void)
{
	u64 x = 1ull << 40;
	unsigned long map[4] = {0, 0, 0, 0};

	CHECK(fls64(x) == 41 && ffs64(x) == 41);
	CHECK(fls(0) == 0 && fls(0x80000000u) == 32 && ffs(0) == 0 && ffs(8) == 4);
	CHECK(hweight64(0xF0F0F0F0F0F0F0F0ull) == 32 && hweight32(0xFFu) == 8);
	CHECK(ilog2(1) == 0 && ilog2(4096) == 12 && ilog2(1ull << 40) == 40);
	CHECK(is_power_of_2(64) && !is_power_of_2(0) && !is_power_of_2(96));
	set_bit(3, map);
	set_bit(BITS_PER_LONG + 1, map);
	CHECK(test_bit(3, map) && test_bit(BITS_PER_LONG + 1, map) && !test_bit(4, map));
	CHECK(test_and_clear_bit(3, map) && !test_bit(3, map));
	CHECK(!test_and_set_bit(5, map) && test_and_set_bit(5, map));
	CHECK(find_first_bit(map, 4 * BITS_PER_LONG) == 5);
	CHECK(find_next_bit(map, 4 * BITS_PER_LONG, 6) == BITS_PER_LONG + 1);
	CHECK(find_next_bit(map, 4 * BITS_PER_LONG, BITS_PER_LONG + 2) == 4 * BITS_PER_LONG);
	CHECK(find_first_zero_bit(map, 4 * BITS_PER_LONG) == 0);
	CHECK(BITS_PER_LONG == (int)(sizeof(long) * 8));
	CHECK(rounddown_64(1ull << 40 | 5, 4096) == 1ull << 40);
	CHECK(howmany_64((1ull << 40) + 1, 4096) == (1ull << 28) + 1);
	CHECK(div_u64(1ull << 40, 3) == 366503875925ull);
	{ u64 n = 1ull << 40; u32 r = do_div(n, 1000); CHECK(r == 776 && n == 1099511627ull); }
	CHECK(log2_if_power2(4096) == 12 && log2_if_power2(100) == -1);
	CHECK(mask64_if_power2(4096) == 4095 && mask64_if_power2(100) == 0);
	CHECK(S_ISDIR(0040755) && S_ISREG(0100644) && S_ISLNK(0120777) && !S_ISDIR(0100644));
	CHECK(min(3, 4) == 3 && max(3u, 4u) == 4 && clamp(9, 1, 5) == 5);
}

static void uuid_helpers(void)
{
	uuid_t a, b;
	u8 raw[16];
	int i;

	for (i = 0; i < 16; i++) raw[i] = (u8)(i + 1);
	memset(&a, 0, sizeof(a));
	CHECK(uuid_is_null(&a) && uuid_equal(&a, &a) && sizeof(uuid_t) == 16);
	import_uuid(&a, raw);
	CHECK(!uuid_is_null(&a));
	uuid_copy(&b, &a);
	CHECK(uuid_equal(&a, &b));
	b.b[15] ^= 1;
	CHECK(!uuid_equal(&a, &b));
}

static void locks_smoke(void)
{
	spinlock_t s; struct rw_semaphore rw; struct mutex m;
	atomic_t a = ATOMIC_INIT(0); atomic64_t a64 = ATOMIC64_INIT(1ll << 40);
	refcount_t rc;

	spin_lock_init(&s); spin_lock(&s); spin_unlock(&s);
	CHECK(spin_trylock(&s)); spin_unlock(&s);
	init_rwsem(&rw);
	down_read(&rw); down_read(&rw); up_read(&rw); up_read(&rw);
	down_write(&rw); CHECK(!down_read_trylock(&rw)); up_write(&rw);
	CHECK(down_read_trylock(&rw)); CHECK(!down_write_trylock(&rw)); up_read(&rw);
	mutex_init(&m); mutex_lock(&m); CHECK(!mutex_trylock(&m)); mutex_unlock(&m);
	CHECK(mutex_trylock(&m)); mutex_unlock(&m);
	mutex_destroy(&m); spin_lock_destroy(&s); destroy_rwsem(&rw);

	atomic_inc(&a); atomic_add(4, &a);
	CHECK(atomic_read(&a) == 5);
	CHECK(atomic_dec_and_test(&a) == 0 && atomic_read(&a) == 4);
	atomic_set(&a, 1); CHECK(atomic_dec_and_test(&a));
	CHECK(atomic64_read(&a64) == (1ll << 40));
	atomic64_add(5, &a64);
	CHECK(atomic64_inc_return(&a64) == (1ll << 40) + 6);
	refcount_set(&rc, 1); refcount_inc(&rc);
	CHECK(!refcount_dec_and_test(&rc) && refcount_dec_and_test(&rc));
	rcu_read_lock(); rcu_read_unlock();
}

#ifndef _WIN32
#include <pthread.h>
struct lk_arg { spinlock_t s; struct mutex m; struct rw_semaphore rw;
		long sctr, mctr, wctr; };
static void *hammer(void *p)
{
	struct lk_arg *a = p;
	int i;
	for (i = 0; i < 20000; i++) {
		spin_lock(&a->s); a->sctr++; spin_unlock(&a->s);
		mutex_lock(&a->m); a->mctr++; mutex_unlock(&a->m);
		down_write(&a->rw); a->wctr++; up_write(&a->rw);
		down_read(&a->rw); (void)a->wctr; up_read(&a->rw);
	}
	return NULL;
}
static void locks_threads(void)
{
	struct lk_arg a;
	pthread_t t[4];
	int i;

	memset(&a, 0, sizeof(a));
	spin_lock_init(&a.s); mutex_init(&a.m); init_rwsem(&a.rw);
	for (i = 0; i < 4; i++) pthread_create(&t[i], NULL, hammer, &a);
	for (i = 0; i < 4; i++) pthread_join(t[i], NULL);
	CHECK(a.sctr == 80000 && a.mctr == 80000 && a.wctr == 80000);
}
#else
static void locks_threads(void) {}
#endif

static void alloc_cap_returns_null_beyond_cap(void)
{
	void *a, *b, *c;
	size_t base = wfs_mem_used();

	wfs_mem_set_cap(base + 4096);
	a = kmalloc(1000, GFP_KERNEL);
	CHECK(a != NULL);
	b = kzalloc(2000, GFP_KERNEL);
	CHECK(b != NULL && ((char *)b)[1999] == 0);
	c = kmalloc(2000, GFP_KERNEL);
	CHECK(c == NULL);
	CHECK(kzalloc((size_t)1 << 40, GFP_KERNEL) == NULL);
	CHECK(wfs_mem_used() >= base + 3000);
	kfree(a);
	c = kmalloc(2000, GFP_KERNEL);
	CHECK(c != NULL);
	kfree(b); kfree(c);
	CHECK(wfs_mem_used() == base);
	wfs_mem_set_cap(0);
	kfree(NULL);

	{ struct kmem_cache *kc = kmem_cache_create("t", 64, 0, 0, NULL);
	  void *o; CHECK(kc != NULL);
	  wfs_mem_set_cap(wfs_mem_used());
	  CHECK(kmem_cache_alloc(kc, GFP_KERNEL) == NULL);
	  wfs_mem_set_cap(0);
	  o = kmem_cache_zalloc(kc, GFP_KERNEL);
	  CHECK(o != NULL && ((char *)o)[63] == 0);
	  kmem_cache_free(kc, o);
	  kmem_cache_destroy(kc); }
	CHECK(wfs_mem_used() == base);
	{ void *n = kmalloc_array((size_t)1 << 62, 8, GFP_KERNEL); CHECK(n == NULL); }
	{ char *r = kvmalloc(10, GFP_KERNEL); CHECK(r != NULL); kvfree(r); }
}

static int logged;
static void log_sink(int level, const char *msg) { (void)level; (void)msg; logged++; }

static void messages_and_assert(void)
{
	wfs_log_set_hook(log_sink);
	xfs_warn(NULL, "value %d", 3);
	xfs_alert(NULL, "x");
	CHECK(logged == 2);
	wfs_log_set_hook(NULL);
	ASSERT(1 == 1);
	CHECK(wfs_assert_fatal() == 0);
	{ int z = 0; trace_xfs_shim_test_event(z, "a"); (void)z; }
}

int main(void)
{
	crc32c_known_vectors();
	crc32c_chunking_and_alignment();
	errno_values_are_linux();
	endian_roundtrip_be16_32_64();
	bitops_are_64bit_safe();
	uuid_helpers();
	locks_smoke();
	locks_threads();
	alloc_cap_returns_null_beyond_cap();
	messages_and_assert();
	printf("test_shim: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
