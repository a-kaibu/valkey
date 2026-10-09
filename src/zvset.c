#include "server.h"
#include "zvset.h"
#include "sds.h"
#include "hashtable.h"
#include "fbtree.h"
#include "zmalloc.h"
#include "util.h"
#include "fpconv_dtoa.h"

#include <string.h>
#include <strings.h>
#include <math.h>
#include <arpa/inet.h>

/* PoC: duplicate of ordered_index.c score normalization.
 * Intentionally not shared (per PoC instructions). */
uint64_t zvScoreToSortable(double score) {
    if (score == 0.0) score = 0.0;
    uint64_t bits;
    memcpy(&bits, &score, sizeof(bits));
    if (bits & (1ULL << 63)) {
        bits = ~bits;
    } else {
        bits ^= (1ULL << 63);
    }
    return htonu64(bits);
}

double zvSortableToScore(uint64_t sortable) {
    uint64_t bits = ntohu64(sortable);
    if (bits & (1ULL << 63)) {
        bits ^= (1ULL << 63);
    } else {
        bits = ~bits;
    }
    double score;
    memcpy(&score, &bits, sizeof(score));
    return score;
}

/* Parse "v0#v1#...#vn" into zvScore. Returns NULL on any error.
 * Rejects: empty string, empty components, >255 components, NaN,
 * invalid floats. Allows +/-inf like ZSET. */
zvScore *zvScoreParse(const char *str, size_t len) {
    if (len == 0) return NULL;

    /* Count components. */
    size_t count = 1;
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '#') count++;
    }
    if (count == 0 || count > ZVSET_MAX_DIMENSIONS) return NULL;

    zvScore *score = zmalloc(sizeof(*score) + sizeof(double) * count);
    score->len = (uint8_t)count;

    size_t idx = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || str[i] == '#') {
            size_t clen = i - start;
            if (clen == 0) {
                zfree(score);
                return NULL;
            }
            double v;
            if (!string2d(str + start, clen, &v)) {
                zfree(score);
                return NULL;
            }
            /* string2d already rejects NaN, double-check. */
            if (isnan(v)) {
                zfree(score);
                return NULL;
            }
            /* Canonicalize -0.0 to +0.0. */
            if (v == 0.0) v = 0.0;
            score->values[idx++] = v;
            start = i + 1;
        }
    }
    serverAssert(idx == count);
    return score;
}

void zvScoreFree(zvScore *score) {
    zfree(score);
}

zvScore *zvScoreCreate(uint8_t len) {
    serverAssert(len >= 1);
    zvScore *score = zmalloc(sizeof(*score) + sizeof(double) * len);
    score->len = len;
    return score;
}

/* Lexicographic compare. Callers must reject mismatched dimensions first. */
int zvScoreCompare(const zvScore *a, const zvScore *b) {
    serverAssert(a->len == b->len);
    for (int i = 0; i < a->len; i++) {
        if (a->values[i] < b->values[i]) return -1;
        if (a->values[i] > b->values[i]) return 1;
    }
    return 0;
}

/* Component-wise addition. NaN (e.g. +inf + -inf) aborts with C_ERR. */
int zvScoreIncrement(zvScore *result, const zvScore *base, const zvScore *delta) {
    serverAssert(result->len == base->len && base->len == delta->len);
    for (int i = 0; i < base->len; i++) {
        double v = base->values[i] + delta->values[i];
        if (isnan(v)) return C_ERR;
        if (v == 0.0) v = 0.0;
        result->values[i] = v;
    }
    return C_OK;
}

/* Format a parsed vector as "v0#v1#...". */
sds zvScoreFormat(const zvScore *score) {
    sds out = sdsempty();
    char buf[128];
    for (int i = 0; i < score->len; i++) {
        if (i > 0) out = sdscatlen(out, "#", 1);
        int len = fpconv_dtoa(score->values[i], buf);
        out = sdscatlen(out, buf, (size_t)len);
    }
    return out;
}

/* --- Score range bounds (BYSCORE) --- */

int zvParseScoreBound(const char *str, size_t len, uint8_t dims, zvScoreBound *bound) {
    bound->unbounded = 0;
    bound->exclusive = 0;
    bound->neg_inf = 0;
    bound->pos_inf = 0;
    bound->score = NULL;
    if (len == 1 && (str[0] == '-' || str[0] == '+')) {
        bound->unbounded = 1;
        if (str[0] == '-')
            bound->neg_inf = 1;
        else
            bound->pos_inf = 1;
        return C_OK;
    }
    if (len > 1 && str[0] == '(') {
        bound->exclusive = 1;
        str++;
        len--;
    }
    zvScore *score = zvScoreParse(str, len);
    if (score == NULL) return C_ERR;
    if (score->len != dims) {
        zvScoreFree(score);
        return C_ERR;
    }
    bound->score = score;
    return C_OK;
}

void zvFreeScoreBound(zvScoreBound *bound) {
    if (bound->score) zvScoreFree(bound->score);
    bound->score = NULL;
}

sds zvBuildScorePrefix(const zvScore *score) {
    size_t total = 1 + (size_t)score->len * 8;
    sds prefix = sdsnewlen(NULL, total);
    prefix[0] = (char)score->len;
    for (int i = 0; i < score->len; i++) {
        uint64_t sortable = zvScoreToSortable(score->values[i]);
        memcpy(prefix + 1 + (size_t)i * 8, &sortable, 8);
    }
    return prefix;
}

/* Byte successor: increment the last non-0xFF byte and TRUNCATE
 * everything after it. The result is the shortest string strictly
 * greater than every string with the input as a prefix, so a carry
 * never leaks into the comparison (e.g. "...00 FF" becomes "...01",
 * which still sorts before "...01 00"). Operates purely on bytes;
 * nothing is decoded. Returns NULL when the input is all 0xFF
 * (unreachable for encoded score prefixes, since NaN is rejected). */
sds zvBoundSuccessor(const_sds bound) {
    size_t len = sdslen(bound);
    for (size_t i = len; i > 0; i--) {
        unsigned char b = (unsigned char)bound[i - 1];
        if (b != 0xFF) {
            sds succ = sdsnewlen(bound, i);
            succ[i - 1] = (char)(b + 1);
            return succ;
        }
    }
    return NULL;
}

void zvScoreRanks(zvset *zs, const_sds lower, const_sds upper, unsigned long *lo, unsigned long *hi) {
    unsigned long len = fbtreeLength(zs->tree);
    fbtreeIterator it;
    if (lower != NULL) {
        fbtreeInitIterator(&it, zs->tree);
        long rank = fbtreeSeekToValue(lower, &it);
        *lo = rank < 0 ? 0 : (unsigned long)rank;
    } else {
        *lo = 0;
    }
    if (upper != NULL) {
        fbtreeInitIterator(&it, zs->tree);
        long rank = fbtreeSeekToValue(upper, &it);
        *hi = rank < 0 ? 0 : (unsigned long)rank;
    } else {
        *hi = len;
    }
    if (*lo > len) *lo = len;
    if (*hi > len) *hi = len;
    if (*lo > *hi) *lo = *hi;
}

void zvIterateRange(zvset *zs, unsigned long lo, unsigned long hi, int reverse, long offset, long count, zvRangeEmit emit, void *ctx) {
    if (lo >= hi) return;
    if (offset < 0) offset = 0;
    fbtreeIterator it;
    fbtreeInitIterator(&it, zs->tree);
    if (!reverse) {
        unsigned long start = lo + (unsigned long)offset;
        if (start >= hi) return;
        unsigned long n = hi - start;
        if (count >= 0 && n > (unsigned long)count) n = (unsigned long)count;
        if (start > 0) fbtreeSeekToRank(&it, start);
        for (unsigned long i = 0; i < n; i++) {
            const_sds item = fbtreeNext(&it);
            if (item == NULL) break;
            emit(ctx, item);
        }
    } else {
        long start = (long)hi - 1 - offset;
        if (start < (long)lo) return;
        unsigned long n = (unsigned long)(start - (long)lo) + 1;
        if (count >= 0 && n > (unsigned long)count) n = (unsigned long)count;
        /* After SeekToRank(r), Prev() yields rank r-1. */
        fbtreeSeekToRank(&it, (unsigned long)start + 1);
        for (unsigned long i = 0; i < n; i++) {
            const_sds item = fbtreePrev(&it);
            if (item == NULL) break;
            emit(ctx, item);
        }
    }
}

/* --- Lex bounds (BYLEX, uniform-vector keys only) --- */

int zvParseLexBound(const char *str, size_t len, zvLexBound *bound) {
    bound->unbounded = 0;
    bound->exclusive = 0;
    bound->neg_inf = 0;
    bound->pos_inf = 0;
    bound->member = NULL;
    if (len == 1 && (str[0] == '-' || str[0] == '+')) {
        bound->unbounded = 1;
        if (str[0] == '-')
            bound->neg_inf = 1;
        else
            bound->pos_inf = 1;
        return C_OK;
    }
    if (len >= 1 && (str[0] == '[' || str[0] == '(')) {
        bound->exclusive = (str[0] == '(');
        bound->member = sdsnewlen(str + 1, len - 1);
        return C_OK;
    }
    return C_ERR;
}

void zvFreeLexBound(zvLexBound *bound) {
    if (bound->member) sdsfree(bound->member);
    bound->member = NULL;
}

int zvLexMemberCompare(const char *a, size_t alen, const char *b, size_t blen) {
    size_t minlen = alen < blen ? alen : blen;
    int cmp = minlen ? memcmp(a, b, minlen) : 0;
    if (cmp != 0) return cmp < 0 ? -1 : 1;
    if (alen == blen) return 0;
    return alen < blen ? -1 : 1;
}

/* Compare the score prefixes ([dims][sortables]) of two packed items. */
static int zvItemPrefixCompare(const_sds a, const_sds b) {
    uint8_t da = (uint8_t)a[0];
    uint8_t db = (uint8_t)b[0];
    if (da != db) return da < db ? -1 : 1;
    size_t prefix = 1 + (size_t)da * 8;
    serverAssert(sdslen(a) >= prefix && sdslen(b) >= prefix);
    int cmp = memcmp(a, b, prefix);
    if (cmp != 0) return cmp < 0 ? -1 : 1;
    return 0;
}

int zvsetUniformVector(zvset *zs) {
    if (fbtreeLength(zs->tree) <= 1) return 1;
    const_sds first = fbtreePeekMin(zs->tree);
    const_sds last = fbtreePeekMax(zs->tree);
    serverAssert(first != NULL && last != NULL);
    if (zvItemPrefixCompare(first, last) != 0) return 0;
    return 1;
}

sds zvLexSeekKey(zvset *zs, const char *member, size_t member_len) {
    const_sds first = fbtreePeekMin(zs->tree);
    serverAssert(first != NULL);
    uint8_t dims = (uint8_t)first[0];
    size_t prefix = 1 + (size_t)dims * 8;
    serverAssert(sdslen(first) >= prefix);
    sds key = sdsnewlen(NULL, prefix + member_len);
    memcpy(key, first, prefix);
    memcpy(key + prefix, member, member_len);
    return key;
}

/* Create packed fbtree item: [dims:u8][sortable...][member]. */
sds zvItemCreate(const zvScore *score, const char *member, size_t member_len) {
    size_t total = 1 + (size_t)score->len * 8 + member_len;
    sds item = sdsnewlen(NULL, total);
    item[0] = (char)score->len;
    for (int i = 0; i < score->len; i++) {
        uint64_t sortable = zvScoreToSortable(score->values[i]);
        memcpy(item + 1 + (size_t)i * 8, &sortable, 8);
    }
    memcpy(item + 1 + (size_t)score->len * 8, member, member_len);
    return item;
}

uint8_t zvItemDimensions(const_sds item) {
    return (uint8_t)item[0];
}

const char *zvItemMember(const_sds item, size_t *member_len) {
    uint8_t dims = (uint8_t)item[0];
    size_t offset = 1 + (size_t)dims * 8;
    serverAssert(sdslen(item) >= offset);
    *member_len = sdslen(item) - offset;
    return item + offset;
}

double zvItemScoreAt(const_sds item, uint8_t dimension) {
    uint8_t dims = (uint8_t)item[0];
    serverAssert(dimension < dims);
    uint64_t sortable;
    memcpy(&sortable, item + 1 + (size_t)dimension * 8, 8);
    return zvSortableToScore(sortable);
}

/* Compare decoded scores. Used for update fast-path. */
int zvItemScoreEquals(const_sds item, const zvScore *score) {
    uint8_t dims = (uint8_t)item[0];
    if (dims != score->len) return 0;
    /* Fast path: memcmp encoded prefix. Encoded form is canonical
     * (-0 normalized at both parse and pack time). */
    size_t prefix = (size_t)dims * 8;
    /* Build encoded buffer on stack for comparison without full decode.
     * For dims <= 32 use stack, else fallback to per-score compare. */
    if (dims <= 32) {
        uint64_t buf[32];
        for (int i = 0; i < dims; i++) buf[i] = zvScoreToSortable(score->values[i]);
        return memcmp(item + 1, buf, prefix) == 0;
    }
    for (int i = 0; i < dims; i++) {
        uint64_t sortable;
        memcpy(&sortable, item + 1 + (size_t)i * 8, 8);
        if (sortable != zvScoreToSortable(score->values[i])) return 0;
    }
    return 1;
}

/* Format vector as "v0#v1#...". Uses fpconv_dtoa like ZSET replies. */
sds zvItemFormatScore(const_sds item) {
    uint8_t dims = (uint8_t)item[0];
    sds out = sdsempty();
    char buf[128];
    for (int i = 0; i < dims; i++) {
        if (i > 0) out = sdscatlen(out, "#", 1);
        double v = zvItemScoreAt(item, (uint8_t)i);
        int len = fpconv_dtoa(v, buf);
        out = sdscatlen(out, buf, (size_t)len);
    }
    return out;
}

/* --- Hashtable support ---
 * Stored entries are packed items; lookup keys are plain SDS marked
 * via zsetMarkLookupKey(). Reuses the ZSET marking mechanism. */

static const char *zvsetExtractElement(const void *key, size_t *len) {
    const_sds s = (const_sds)key;
    if (zsetIsLookupKey(s)) {
        unsigned char flags = s[-1];
        if ((flags & SDS_TYPE_MASK) == ZSET_LOOKUP_TYPE5_MARKER) {
            *len = flags >> SDS_TYPE_BITS;
        } else {
            *len = sdslen(s);
        }
        return (const char *)s;
    }
    size_t member_len;
    const char *member = zvItemMember(s, &member_len);
    *len = member_len;
    return member;
}

static uint64_t zvsetHashFunction(const void *key) {
    size_t len;
    const char *ptr = zvsetExtractElement(key, &len);
    return genHashFunctionConfigurableSeed(ptr, len);
}

static int zvsetKeyCompare(const void *a, const void *b) {
    size_t alen, blen;
    const char *aptr = zvsetExtractElement(a, &alen);
    const char *bptr = zvsetExtractElement(b, &blen);
    if (alen != blen) return 0;
    return memcmp(aptr, bptr, alen) == 0;
}

hashtableType zvsetHashtableType = {
    .hashFunction = zvsetHashFunction,
    .keyCompare = zvsetKeyCompare,
};

/* --- Core operations (modelled on zsetAdd BTREE branch) --- */

void *zvsetFind(zvset *zs, sds member) {
    void *entry = NULL;
    zsetMarkLookupKey(member);
    hashtableFind(zs->ht, member, &entry);
    zsetUnmarkLookupKey(member);
    return entry;
}

/* Replace the item referenced by the hashtable slot with a new score.
 * The old item is deleted from the tree before the new one is inserted,
 * then the slot pointer is updated. Returns the inserted item. */
static sds zvsetReplaceItem(zvset *zs, void **item_ref, const zvScore *score, sds member) {
    const_sds old_item = *item_ref;
    sds new_item = zvItemCreate(score, member, sdslen(member));
    serverAssert(fbtreeDelete(zs->tree, old_item));
    sds inserted = fbtreeInsert(zs->tree, new_item);
    *item_ref = inserted;
    return inserted;
}

/* Read the full vector of a packed item into out (len == dims). */
static void zvsetItemToScore(const_sds item, zvScore *out) {
    uint8_t dims = (uint8_t)item[0];
    serverAssert(out->len == dims);
    for (int i = 0; i < dims; i++) {
        uint64_t sortable;
        memcpy(&sortable, item + 1 + (size_t)i * 8, 8);
        out->values[i] = zvSortableToScore(sortable);
    }
}

void zvItemToScore(const_sds item, zvScore *out) {
    zvsetItemToScore(item, out);
}

void zvScoreApplyWeight(zvScore *out, const zvScore *v, double weight) {
    serverAssert(out->len == v->len);
    for (int i = 0; i < v->len; i++) {
        double x = v->values[i] * weight;
        if (isnan(x)) x = 0;
        if (x == 0.0) x = 0.0;
        out->values[i] = x;
    }
}

void zvScoreAggregate(zvScore *acc, const zvScore *weighted, int aggregate) {
    serverAssert(acc->len == weighted->len);
    if (aggregate == ZV_AGGR_SUM) {
        for (int i = 0; i < acc->len; i++) {
            double x = acc->values[i] + weighted->values[i];
            if (isnan(x)) x = 0;
            if (x == 0.0) x = 0.0;
            acc->values[i] = x;
        }
        return;
    }
    int cmp = zvScoreCompare(weighted, acc);
    int take_weighted = (aggregate == ZV_AGGR_MIN) ? (cmp < 0) : (cmp > 0);
    if (take_weighted) {
        for (int i = 0; i < acc->len; i++) acc->values[i] = weighted->values[i];
    }
}

int zvsetAdd(zvset *zs, const zvScore *score, sds member, int in_flags, int *out_flags) {
    int nx = in_flags & ZVADD_IN_NX;
    int xx = in_flags & ZVADD_IN_XX;
    int gt = in_flags & ZVADD_IN_GT;
    int lt = in_flags & ZVADD_IN_LT;

    *out_flags = 0;

    if (score->len != zs->dimensions) return C_ERR;

    zsetMarkLookupKey(member);
    void **item_ref = hashtableFindRef(zs->ht, member);
    zsetUnmarkLookupKey(member);

    if (item_ref != NULL) {
        if (nx) {
            *out_flags |= ZVADD_OUT_NOP;
            return C_OK;
        }
        const_sds old_item = *item_ref;
        if (zvItemScoreEquals(old_item, score)) return C_OK;

        /* GT/LT compare the full vector lexicographically. */
        if (gt || lt) {
            zvScore *cur = zvScoreCreate(zs->dimensions);
            zvsetItemToScore(old_item, cur);
            int cmp = zvScoreCompare(score, cur);
            zvScoreFree(cur);
            if ((gt && cmp <= 0) || (lt && cmp >= 0)) {
                *out_flags |= ZVADD_OUT_NOP;
                return C_OK;
            }
        }

        zvsetReplaceItem(zs, item_ref, score, member);
        *out_flags |= ZVADD_OUT_UPDATED;
        return C_OK;
    }

    if (xx) {
        *out_flags |= ZVADD_OUT_NOP;
        return C_OK;
    }

    sds item = zvItemCreate(score, member, sdslen(member));
    sds inserted = fbtreeInsert(zs->tree, item);
    serverAssert(hashtableAdd(zs->ht, inserted));
    *out_flags |= ZVADD_OUT_ADDED;
    return C_OK;
}

int zvsetIncrBy(zvset *zs, const zvScore *delta, sds member, int in_flags, int *out_flags, zvScore *newscore) {
    int nx = in_flags & ZVADD_IN_NX;
    int xx = in_flags & ZVADD_IN_XX;
    int gt = in_flags & ZVADD_IN_GT;
    int lt = in_flags & ZVADD_IN_LT;

    *out_flags = 0;

    if (delta->len != zs->dimensions || newscore->len != zs->dimensions) return C_ERR;

    zsetMarkLookupKey(member);
    void **item_ref = hashtableFindRef(zs->ht, member);
    zsetUnmarkLookupKey(member);

    if (item_ref != NULL) {
        if (nx) {
            *out_flags |= ZVADD_OUT_NOP;
            return C_OK;
        }
        zvScore *cur = zvScoreCreate(zs->dimensions);
        zvsetItemToScore(*item_ref, cur);
        if (zvScoreIncrement(newscore, cur, delta) != C_OK) {
            zvScoreFree(cur);
            return C_ERR;
        }
        int cmp = zvScoreCompare(newscore, cur);
        zvScoreFree(cur);
        if ((gt && cmp <= 0) || (lt && cmp >= 0)) {
            *out_flags |= ZVADD_OUT_NOP;
            return C_OK;
        }
        /* Skip delete/insert when the result is identical. */
        if (zvItemScoreEquals(*item_ref, newscore)) return C_OK;
        zvsetReplaceItem(zs, item_ref, newscore, member);
        *out_flags |= ZVADD_OUT_UPDATED;
        return C_OK;
    }

    if (xx) {
        *out_flags |= ZVADD_OUT_NOP;
        return C_OK;
    }

    /* Missing member starts from the zero vector. */
    for (int i = 0; i < delta->len; i++) {
        if (isnan(delta->values[i])) return C_ERR;
        newscore->values[i] = delta->values[i] == 0.0 ? 0.0 : delta->values[i];
    }
    sds item = zvItemCreate(newscore, member, sdslen(member));
    sds inserted = fbtreeInsert(zs->tree, item);
    serverAssert(hashtableAdd(zs->ht, inserted));
    *out_flags |= ZVADD_OUT_ADDED;
    return C_OK;
}

int zvsetDel(zvset *zs, sds member) {
    void *item = NULL;
    zsetMarkLookupKey(member);
    int found = hashtablePop(zs->ht, member, &item);
    zsetUnmarkLookupKey(member);
    if (!found) return 0;
    /* hashtable holds a non-owning pointer; fbtree owns and frees it. */
    serverAssert(fbtreeDelete(zs->tree, item));
    return 1;
}

unsigned long zvsetLength(const zvset *zs) {
    return fbtreeLength((fbtreeIndex *)zs->tree);
}

/* Duplicate a zvset object (for COPY). Same encoding guarantee. */
robj *zvsetDup(robj *o) {
    serverAssert(objectGetType(o) == OBJ_ZVSET);
    zvset *zs = objectGetVal(o);
    robj *zobj = createZvsetObject(zs->dimensions);
    zvset *new_zs = objectGetVal(zobj);
    hashtableExpand(new_zs->ht, hashtableSize(zs->ht));
    /* Copy from greatest to smallest: optimal for fbtree inserts. */
    fbtreeIterator iter;
    fbtreeInitIterator(&iter, zs->tree);
    const_sds item;
    while ((item = fbtreePrev(&iter)) != NULL) {
        sds copy = sdsdup(item);
        sds inserted = fbtreeInsert(new_zs->tree, copy);
        serverAssert(hashtableAdd(new_zs->ht, inserted));
    }
    return zobj;
}
