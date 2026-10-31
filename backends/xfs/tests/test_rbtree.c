// SPDX-License-Identifier: GPL-2.0-only
/* Kernel rbtree (third_party/linux-lib, LLP64 patch) through the kernel API. */
#include "xfs_platform.h"
#include <linux/rbtree_augmented.h>	/* rb_is_red, rb_parent */
#include <stdio.h>
#include <stdlib.h>

static int failures, checks;
#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

struct item { struct rb_node rb; u64 key; int in; };

static void insert(struct rb_root *root, struct item *it)
{
	struct rb_node **p = &root->rb_node, *parent = NULL;

	while (*p) {
		struct item *c = rb_entry(*p, struct item, rb);

		parent = *p;
		p = it->key < c->key ? &(*p)->rb_left : &(*p)->rb_right;
	}
	rb_link_node(&it->rb, parent, p);
	rb_insert_color(&it->rb, root);
}

/* Black height of every path equal, no red node with a red child. */
static int black_height(struct rb_node *n, int *ok)
{
	int l, r;

	if (!n)
		return 1;
	if (rb_is_red(n) && ((n->rb_left && rb_is_red(n->rb_left)) ||
			     (n->rb_right && rb_is_red(n->rb_right))))
		*ok = 0;
	if ((n->rb_left && rb_parent(n->rb_left) != n) ||
	    (n->rb_right && rb_parent(n->rb_right) != n))
		*ok = 0;
	l = black_height(n->rb_left, ok);
	r = black_height(n->rb_right, ok);
	if (l != r)
		*ok = 0;
	return l + !rb_is_red(n);
}

static int count_sorted(struct rb_root *root, int *sorted)
{
	struct rb_node *n;
	u64 prev = 0;
	int cnt = 0;

	*sorted = 1;
	for (n = rb_first(root); n; n = rb_next(n)) {
		struct item *it = rb_entry(n, struct item, rb);

		if (cnt && it->key < prev)
			*sorted = 0;
		prev = it->key;
		cnt++;
	}
	return cnt;
}

int main(void)
{
	enum { N = 20000 };
	struct item *items = calloc(N, sizeof(*items));
	struct rb_root root = RB_ROOT;
	int i, ok = 1, sorted, live = 0;

	srand(1);
	CHECK(RB_EMPTY_ROOT(&root));
	for (i = 0; i < N; i++) {
		/* High bits set too: pointers and keys both exercise 64 bits. */
		items[i].key = ((u64)rand() << 33) ^ (u64)rand();
		insert(&root, &items[i]);
		items[i].in = 1;
		live++;
	}
	black_height(root.rb_node, &ok);
	CHECK(ok);
	CHECK(count_sorted(&root, &sorted) == N && sorted);
	for (i = 0; i < N; i += 2) {
		rb_erase(&items[i].rb, &root);
		items[i].in = 0;
		live--;
	}
	ok = 1;
	black_height(root.rb_node, &ok);
	CHECK(ok);
	CHECK(count_sorted(&root, &sorted) == live && sorted);
	for (i = 1; i < N; i += 2)
		rb_erase(&items[i].rb, &root);
	CHECK(RB_EMPTY_ROOT(&root));
	free(items);
	printf("test_rbtree: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
