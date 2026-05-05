#include "art.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *items[64];
    int count;
} KeyList;

typedef struct {
    int count;
} CountCtx;

typedef struct {
    const unsigned char *expected;
    int count;
} ByteOrderCtx;

static int freed_values;

static void count_free_value(void *value) {
    (void)value;
    freed_values++;
}

static int collect_keys(const unsigned char *key, size_t key_len, void *value, void *ctx) {
    KeyList *list = ctx;
    char *copy;

    (void)value;
    assert(list->count < 64);
    copy = malloc(key_len + 1);
    assert(copy != NULL);
    memcpy(copy, key, key_len);
    copy[key_len] = '\0';
    list->items[list->count++] = copy;
    return 0;
}

static int count_key(const unsigned char *key, size_t key_len, void *value, void *ctx) {
    CountCtx *count = ctx;

    (void)key;
    (void)key_len;
    (void)value;
    count->count++;
    return 0;
}

static int assert_single_byte_order(const unsigned char *key, size_t key_len, void *value, void *ctx) {
    ByteOrderCtx *order = ctx;

    (void)value;
    assert(key_len == 1);
    assert(key[0] == order->expected[order->count]);
    order->count++;
    return 0;
}

static void free_keys(KeyList *list) {
    int i;

    for (i = 0; i < list->count; i++) {
        free((void *)list->items[i]);
    }
    list->count = 0;
}

static void assert_keys(KeyList *list, const char **expected, int expected_count) {
    int i;

    assert(list->count == expected_count);
    for (i = 0; i < expected_count; i++) {
        assert(strcmp(list->items[i], expected[i]) == 0);
    }
}

static void test_insert_search_replace(void) {
    ArtTree *tree = art_create(NULL);
    void *old = NULL;

    assert(tree != NULL);
    assert(art_insert(tree, (const unsigned char *)"apple", 5, "1", &old) == 1);
    assert(old == NULL);
    assert(art_insert(tree, (const unsigned char *)"app", 3, "2", &old) == 1);
    assert(art_insert(tree, (const unsigned char *)"banana", 6, "3", &old) == 1);
    assert(art_insert(tree, (const unsigned char *)"band", 4, "4", &old) == 1);
    assert(art_insert(tree, (const unsigned char *)"bandana", 7, "5", &old) == 1);
    assert(art_size(tree) == 5);

    assert(strcmp(art_search(tree, (const unsigned char *)"apple", 5), "1") == 0);
    assert(strcmp(art_search(tree, (const unsigned char *)"app", 3), "2") == 0);
    assert(art_search(tree, (const unsigned char *)"ap", 2) == NULL);

    assert(art_insert(tree, (const unsigned char *)"app", 3, "updated", &old) == 0);
    assert(strcmp(old, "2") == 0);
    assert(strcmp(art_search(tree, (const unsigned char *)"app", 3), "updated") == 0);
    assert(art_size(tree) == 5);

    art_destroy(tree);
}

static void test_iteration_range_prefix(void) {
    ArtTree *tree = art_create(NULL);
    KeyList list = {0};
    const char *ordered[] = {"app", "apple", "banana", "band", "bandana"};
    const char *reverse[] = {"bandana", "band", "banana", "apple", "app"};
    const char *range[] = {"banana", "band", "bandana"};
    const char *prefix[] = {"banana", "band", "bandana"};

    assert(art_insert(tree, (const unsigned char *)"banana", 6, "3", NULL) == 1);
    assert(art_insert(tree, (const unsigned char *)"apple", 5, "1", NULL) == 1);
    assert(art_insert(tree, (const unsigned char *)"bandana", 7, "5", NULL) == 1);
    assert(art_insert(tree, (const unsigned char *)"app", 3, "2", NULL) == 1);
    assert(art_insert(tree, (const unsigned char *)"band", 4, "4", NULL) == 1);

    assert(art_iter(tree, collect_keys, &list) == 0);
    assert_keys(&list, ordered, 5);
    free_keys(&list);

    assert(art_reverse_iter(tree, collect_keys, &list) == 0);
    assert_keys(&list, reverse, 5);
    free_keys(&list);

    assert(art_range(tree,
                     (const unsigned char *)"banana",
                     6,
                     (const unsigned char *)"bandz",
                     5,
                     0,
                     ART_LIMIT_UNLIMITED,
                     collect_keys,
                     &list) == 0);
    assert_keys(&list, range, 3);
    free_keys(&list);

    assert(art_prefix(tree, (const unsigned char *)"ban", 3, ART_LIMIT_UNLIMITED, collect_keys, &list) == 0);
    assert_keys(&list, prefix, 3);
    free_keys(&list);

    assert(art_prefix(tree, (const unsigned char *)"ban", 3, 2, collect_keys, &list) == 0);
    assert_keys(&list, prefix, 2);
    free_keys(&list);

    art_destroy(tree);
}

static void test_delete_and_empty_key(void) {
    ArtTree *tree = art_create(NULL);
    void *old = NULL;
    KeyList list = {0};
    const char *expected[] = {"", "apple"};

    assert(art_insert(tree, (const unsigned char *)"", 0, "empty", NULL) == 1);
    assert(art_insert(tree, (const unsigned char *)"app", 3, "app", NULL) == 1);
    assert(art_insert(tree, (const unsigned char *)"apple", 5, "apple", NULL) == 1);
    assert(strcmp(art_search(tree, (const unsigned char *)"", 0), "empty") == 0);

    assert(art_delete(tree, (const unsigned char *)"app", 3, &old) == 1);
    assert(strcmp(old, "app") == 0);
    assert(art_search(tree, (const unsigned char *)"app", 3) == NULL);
    assert(strcmp(art_search(tree, (const unsigned char *)"apple", 5), "apple") == 0);
    assert(art_size(tree) == 2);

    assert(art_iter(tree, collect_keys, &list) == 0);
    assert_keys(&list, expected, 2);
    free_keys(&list);

    assert(art_delete(tree, (const unsigned char *)"missing", 7, &old) == 0);
    assert(art_size(tree) == 2);

    art_destroy(tree);
}

static void test_range_boundaries(void) {
    ArtTree *tree = art_create(NULL);
    KeyList list = {0};
    const char *single[] = {"a"};
    const char *aa_single[] = {"aa"};
    const char *empty[] = {""};
    const char *from_a_to_z[] = {"a", "aa", "b", "z"};
    const char *reverse_a_to_z[] = {"z", "b", "aa", "a"};
    const char *none[] = {"unused"};

    assert(tree != NULL);
    assert(art_insert(tree, (const unsigned char *)"", 0, "empty", NULL) == 1);
    assert(art_insert(tree, (const unsigned char *)"a", 1, "a", NULL) == 1);
    assert(art_insert(tree, (const unsigned char *)"aa", 2, "aa", NULL) == 1);
    assert(art_insert(tree, (const unsigned char *)"b", 1, "b", NULL) == 1);
    assert(art_insert(tree, (const unsigned char *)"z", 1, "z", NULL) == 1);

    assert(art_range(tree,
                     (const unsigned char *)"a",
                     1,
                     (const unsigned char *)"a",
                     1,
                     0,
                     ART_LIMIT_UNLIMITED,
                     collect_keys,
                     &list) == 0);
    assert_keys(&list, single, 1);
    free_keys(&list);

    assert(art_range(tree,
                     (const unsigned char *)"aa",
                     2,
                     (const unsigned char *)"aa",
                     2,
                     0,
                     ART_LIMIT_UNLIMITED,
                     collect_keys,
                     &list) == 0);
    assert_keys(&list, aa_single, 1);
    free_keys(&list);

    assert(art_range(tree,
                     (const unsigned char *)"",
                     0,
                     (const unsigned char *)"",
                     0,
                     0,
                     ART_LIMIT_UNLIMITED,
                     collect_keys,
                     &list) == 0);
    assert_keys(&list, empty, 1);
    free_keys(&list);

    assert(art_range(tree,
                     (const unsigned char *)"a",
                     1,
                     (const unsigned char *)"z",
                     1,
                     0,
                     ART_LIMIT_UNLIMITED,
                     collect_keys,
                     &list) == 0);
    assert_keys(&list, from_a_to_z, 4);
    free_keys(&list);

    assert(art_range(tree,
                     (const unsigned char *)"a",
                     1,
                     (const unsigned char *)"z",
                     1,
                     1,
                     ART_LIMIT_UNLIMITED,
                     collect_keys,
                     &list) == 0);
    assert_keys(&list, reverse_a_to_z, 4);
    free_keys(&list);

    assert(art_range(tree,
                     (const unsigned char *)"c",
                     1,
                     (const unsigned char *)"d",
                     1,
                     0,
                     ART_LIMIT_UNLIMITED,
                     collect_keys,
                     &list) == 0);
    assert_keys(&list, none, 0);

    assert(art_range(tree,
                     (const unsigned char *)"ab",
                     2,
                     (const unsigned char *)"az",
                     2,
                     0,
                     ART_LIMIT_UNLIMITED,
                     collect_keys,
                     &list) == 0);
    assert_keys(&list, none, 0);

    art_destroy(tree);
}

static void test_deep_iteration_uses_explicit_stack(void) {
    enum { DEPTH = 5000 };
    ArtTree *tree = art_create(NULL);
    CountCtx count = {0};
    unsigned char *key = malloc(DEPTH);
    int i;

    assert(tree != NULL);
    assert(key != NULL);
    memset(key, 'x', DEPTH);

    for (i = 1; i <= DEPTH; i++) {
        assert(art_insert(tree, key, (size_t)i, "v", NULL) == 1);
    }

    assert(strcmp(art_search(tree, key, DEPTH), "v") == 0);
    assert(art_iter(tree, count_key, &count) == 0);
    assert(count.count == DEPTH);

    count.count = 0;
    assert(art_reverse_iter(tree, count_key, &count) == 0);
    assert(count.count == DEPTH);

    free(key);
    art_destroy(tree);
}

static void test_wide_node_delete_keeps_order(void) {
    ArtTree *tree = art_create(NULL);
    unsigned char key;
    unsigned char expected[] = {10, 20, 30};
    ByteOrderCtx order = {expected, 0};
    int i;

    assert(tree != NULL);
    for (i = 1; i <= 70; i++) {
        key = (unsigned char)i;
        assert(art_insert(tree, &key, 1, "v", NULL) == 1);
    }
    for (i = 1; i <= 70; i++) {
        if (i == 10 || i == 20 || i == 30) {
            continue;
        }
        key = (unsigned char)i;
        assert(art_delete(tree, &key, 1, NULL) == 1);
    }

    assert(art_size(tree) == 3);
    for (i = 0; i < 3; i++) {
        assert(strcmp(art_search(tree, &expected[i], 1), "v") == 0);
    }
    assert(art_iter(tree, assert_single_byte_order, &order) == 0);
    assert(order.count == 3);

    art_destroy(tree);
}

static void test_delete_without_old_value_frees_value(void) {
    ArtTree *tree = art_create(count_free_value);

    freed_values = 0;
    assert(tree != NULL);
    assert(art_insert(tree, (const unsigned char *)"a", 1, "a", NULL) == 1);
    assert(art_delete(tree, (const unsigned char *)"a", 1, NULL) == 1);
    assert(freed_values == 1);
    assert(art_size(tree) == 0);

    art_destroy(tree);
}

int main(void) {
    test_insert_search_replace();
    test_iteration_range_prefix();
    test_delete_and_empty_key();
    test_range_boundaries();
    test_deep_iteration_uses_explicit_stack();
    test_wide_node_delete_keeps_order();
    test_delete_without_old_value_frees_value();
    puts("test_art: ok");
    return 0;
}
