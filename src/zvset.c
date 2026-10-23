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

/* Fast path for the common integer component: an optional '-' and at most
 * 15 digits, which a double holds exactly. Returns 0 for anything else,
 * which then goes through string2d(). */
static inline int zvParseSmallInteger(const char *s, size_t len, double *out) {
    size_t i = s[0] == '-';
    if (len == i || len - i > 15) return 0;
    int64_t v = 0;
    for (size_t j = i; j < len; j++) {
        unsigned d = (unsigned char)s[j] - '0';
        if (d > 9) return 0;
        v = v * 10 + d;
    }
    *out = (double)(i ? -v : v);
    return 1;
}

/* Parse "v0#v1#...#vn" into out (room for ZVSET_MAX_DIMENSIONS values).
 * Returns the number of components, or 0 on any error: empty string or
 * component, more than ZVSET_MAX_DIMENSIONS components, NaN or an invalid
 * float. +/-inf are allowed like ZSET; -0 is canonicalized to +0. */
int zvScoreParseInto(const char *str, size_t len, double *out) {
    if (len == 0) return 0;
    int count = 0;
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i < len && str[i] != '#') continue;
        double v;
        if (i == start || count == ZVSET_MAX_DIMENSIONS) return 0;
        if (!zvParseSmallInteger(str + start, i - start, &v) && (!string2d(str + start, i - start, &v) || isnan(v)))
            return 0;
        out[count++] = v == 0.0 ? 0.0 : v;
        start = i + 1;
    }
    return count;
}

zvScore *zvScoreParse(const char *str, size_t len) {
    double values[ZVSET_MAX_DIMENSIONS];
    int count = zvScoreParseInto(str, len, values);
    if (count == 0) return NULL;
    zvScore *score = zvScoreCreate((uint8_t)count);
    memcpy(score->values, values, sizeof(double) * (size_t)count);
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

/* Format a parsed vector as "v0#v1#..." into buf (at least
 * ZVSET_SCORE_BUFLEN bytes). Returns the formatted length. */
/* Format one component like ZSET replies (d2string): integral values use
 * the much faster integer printer, everything else the shortest
 * round-trip representation. -0 never reaches here (canonicalized). */
static inline size_t zvFormatDouble(double v, char *buf) {
    long long ll;
    if (double2ll(v, &ll)) return ll2string(buf, 21, ll);
    return fpconv_dtoa(v, buf);
}

size_t zvScoreFormatBuf(const zvScore *score, char *buf) {
    size_t len = 0;
    for (int i = 0; i < score->len; i++) {
        if (i > 0) buf[len++] = '#';
        len += zvFormatDouble(score->values[i], buf + len);
    }
    return len;
}

sds zvScoreFormat(const zvScore *score) {
    char buf[ZVSET_SCORE_BUFLEN];
    return sdsnewlen(buf, zvScoreFormatBuf(score, buf));
}

/* --- Score range bounds (BYSCORE) --- */

int zvParseScoreBound(const char *str, size_t len, zvScoreBound *bound) {
    bound->exclusive = 0;
    bound->neg_inf = 0;
    bound->pos_inf = 0;
    bound->len = 0;
    if (len == 1 && (str[0] == '-' || str[0] == '+')) {
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
    int count = zvScoreParseInto(str, len, bound->values);
    if (count == 0) return C_ERR;
    bound->len = (uint8_t)count;
    return C_OK;
}

/* Check a parsed bound against the key's dimensions. A scalar infinity
 * ("-inf", "+inf") stands for the vector with every component at that
 * infinity, so TairZset style bounds work on multi-dimensional keys. */
static int zvScoreBoundFit(zvScoreBound *bound, uint8_t dims) {
    if (bound->len == 0 || bound->len == dims) return C_OK;
    if (bound->len != 1 || !isinf(bound->values[0])) return C_ERR;
    for (int i = 1; i < dims; i++) bound->values[i] = bound->values[0];
    bound->len = dims;
    return C_OK;
}

/* Encoded score prefix [dims][sortables] of values, written to buf
 * (1 + 8 * ZVSET_MAX_DIMENSIONS bytes). Returns its length. */
static size_t zvEncodePrefix(const double *values, uint8_t dims, char *buf) {
    buf[0] = (char)dims;
    for (int i = 0; i < dims; i++) {
        uint64_t sortable = zvScoreToSortable(values[i]);
        memcpy(buf + 1 + (size_t)i * 8, &sortable, 8);
    }
    return 1 + (size_t)dims * 8;
}

/* In-place byte successor of buf[0..len): increment the last non-0xFF
 * byte and truncate after it. Returns the new length, or 0 when every
 * byte is 0xFF (unreachable for encoded prefixes, since NaN is rejected). */
static size_t zvSuccessorInPlace(char *buf, size_t len) {
    for (size_t i = len; i > 0; i--) {
        if ((unsigned char)buf[i - 1] != 0xFF) {
            buf[i - 1]++;
            return i;
        }
    }
    return 0;
}

/* Byte successor: increment the last non-0xFF byte and TRUNCATE
 * everything after it. The result is the shortest string strictly
 * greater than every string with the input as a prefix, so a carry
 * never leaks into the comparison (e.g. "...00 FF" becomes "...01",
 * which still sorts before "...01 00"). Operates purely on bytes;
 * nothing is decoded. Returns NULL when the input is all 0xFF
 * (unreachable for encoded score prefixes, since NaN is rejected). */
sds zvBoundSuccessor(const_sds bound) {
    sds succ = sdsdup(bound);
    size_t len = zvSuccessorInPlace(succ, sdslen(succ));
    if (len == 0) {
        sdsfree(succ);
        return NULL;
    }
    sdssetlen(succ, len);
    return succ;
}

/* --- Container: compact and btree encodings ---
 *
 * Everything below this section reaches the items only through these
 * helpers, so the rest of the type is encoding independent. */

static inline int zvsetFitsCompact(unsigned long count, size_t member_len) {
    return count <= server.zset_max_listpack_entries && member_len <= server.zset_max_listpack_value;
}

static inline size_t zvItemMemberLen(const_sds item) {
    return sdslen(item) - 1 - (size_t)(uint8_t)item[0] * 8;
}

/* Compact storage is one allocation of capacity slots: the item
 * pointers, followed by a 16-bit hash tag of each item's member. Member
 * lookups scan the contiguous tags and only dereference items whose tag
 * matches, instead of chasing every item pointer. */
#define ZV_SLOT_SIZE (sizeof(sds) + sizeof(uint16_t))

static inline uint16_t *zvCompactTags(const zvset *zs) {
    return (uint16_t *)(zs->items + zs->capacity);
}

/* FNV-1a, folded to 16 bits. */
static inline uint16_t zvMemberTag(const char *member, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) h = (h ^ (uint8_t)member[i]) * 16777619u;
    return (uint16_t)(h ^ (h >> 16));
}

static inline uint16_t zvItemTag(const_sds item) {
    size_t len;
    const char *member = zvItemMember(item, &len);
    return zvMemberTag(member, len);
}

/* Reallocate to room for at least want slots (want >= count), keeping
 * the tags right after the pointers. */
static void zvCompactResize(zvset *zs, uint32_t want) {
    if (want == 0) {
        zfree(zs->items);
        zs->items = NULL;
        zs->capacity = 0;
        return;
    }
    size_t tags_bytes = sizeof(uint16_t) * zs->count;
    size_t tags_at = sizeof(sds) * zs->capacity;
    if (want < zs->capacity) {
        /* Shrinking: move the tags down before the tail is cut off. */
        memmove((char *)zs->items + sizeof(sds) * want, (char *)zs->items + tags_at, tags_bytes);
        tags_at = sizeof(sds) * want;
    }
    size_t usable;
    char *buf = zrealloc_usable(zs->items, ZV_SLOT_SIZE * want, &usable);
    uint32_t capacity = (uint32_t)(usable / ZV_SLOT_SIZE);
    memmove(buf + sizeof(sds) * capacity, buf + tags_at, tags_bytes);
    zs->items = (sds *)buf;
    zs->capacity = capacity;
}

/* First index whose item is >= key, in the tree's sdscmp() order. */
static unsigned long zvCompactLowerBound(const zvset *zs, const char *key, size_t keylen) {
    unsigned long lo = 0, hi = zs->count;
    while (lo < hi) {
        unsigned long mid = lo + (hi - lo) / 2;
        const_sds item = zs->items[mid];
        if (zvLexMemberCompare(item, sdslen(item), key, keylen) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* Index of the item holding member, or -1. */
static long zvCompactFindMember(const zvset *zs, const char *member, size_t len) {
    const uint16_t *tags = zvCompactTags(zs);
    uint16_t tag = zvMemberTag(member, len);
    /* Items share the key's score width, so the length check rejects most
     * tag collisions without a memcmp. */
    size_t prefix = 1 + (size_t)zs->dimensions * 8;
    size_t itemlen = prefix + len;
    for (uint32_t i = 0; i < zs->count; i++) {
        if (tags[i] != tag) continue;
        const_sds item = zs->items[i];
        if (sdslen(item) == itemlen && memcmp(item + prefix, member, len) == 0) return (long)i;
    }
    return -1;
}

static void zvCompactInsertAt(zvset *zs, unsigned long idx, sds item) {
    if (zs->count == zs->capacity) zvCompactResize(zs, zs->count + 1);
    uint16_t *tags = zvCompactTags(zs);
    memmove(&zs->items[idx + 1], &zs->items[idx], sizeof(sds) * (zs->count - idx));
    memmove(&tags[idx + 1], &tags[idx], sizeof(uint16_t) * (zs->count - idx));
    zs->items[idx] = item;
    tags[idx] = zvItemTag(item);
    zs->count++;
}

/* Drop n slots at idx (the items are not freed). */
static void zvCompactRemoveAt(zvset *zs, unsigned long idx, unsigned long n) {
    uint16_t *tags = zvCompactTags(zs);
    memmove(&zs->items[idx], &zs->items[idx + n], sizeof(sds) * (zs->count - idx - n));
    memmove(&tags[idx], &tags[idx + n], sizeof(uint16_t) * (zs->count - idx - n));
    zs->count -= (uint32_t)n;
    if (zs->count < zs->capacity / 2) zvCompactResize(zs, zs->count);
}

/* Take ownership of count sorted items. */
static void zvCompactAdopt(zvset *zs, sds *items, uint32_t count) {
    zs->items = NULL;
    zs->count = zs->capacity = 0;
    if (count == 0) return;
    zvCompactResize(zs, count);
    memcpy(zs->items, items, sizeof(sds) * count);
    zs->count = count;
    uint16_t *tags = zvCompactTags(zs);
    for (uint32_t i = 0; i < count; i++) tags[i] = zvItemTag(items[i]);
}

static void zvsetConvertToBtree(zvset *zs) {
    serverAssert(zvsetIsCompact(zs));
    hashtable *ht = hashtableCreate(&zvsetHashtableType);
    fbtreeIndex *tree = fbtreeCreate();
    if (zs->count) hashtableExpand(ht, zs->count);
    /* Ascending inserts take fbtree's append path and leave full leaves. */
    for (uint32_t i = 0; i < zs->count; i++) {
        sds inserted = fbtreeInsert(tree, zs->items[i]);
        serverAssert(hashtableAdd(ht, inserted));
    }
    zfree(zs->items);
    zs->items = NULL;
    zs->count = zs->capacity = 0;
    zs->ht = ht;
    zs->tree = tree;
}

zvset *zvsetCreate(uint8_t dimensions) {
    zvset *zs = zcalloc(sizeof(*zs));
    zs->dimensions = dimensions;
    return zs;
}

zvset *zvsetCreateSized(uint8_t dimensions, unsigned long size_hint) {
    zvset *zs = zvsetCreate(dimensions);
    if (!zvsetFitsCompact(size_hint, 0)) zvsetConvertToBtree(zs);
    return zs;
}

void zvsetRelease(zvset *zs) {
    if (zvsetIsCompact(zs)) {
        for (uint32_t i = 0; i < zs->count; i++) sdsfree(zs->items[i]);
        zfree(zs->items);
    } else {
        /* The hashtable holds non-owning pointers; fbtree owns the items. */
        hashtableRelease(zs->ht);
        fbtreeFree(zs->tree);
    }
    zfree(zs);
}

const char *zvsetEncodingName(const zvset *zs) {
    return zvsetIsCompact(zs) ? "compact" : "btree";
}

int zvsetTryExpand(zvset *zs, unsigned long size) {
    if (zvsetIsCompact(zs)) return 1;
    return hashtableTryExpand(zs->ht, size);
}

unsigned long zvsetLength(const zvset *zs) {
    return zvsetIsCompact(zs) ? zs->count : fbtreeLength(zs->tree);
}

void zvsetInsertNew(zvset *zs, sds item) {
    if (zvsetIsCompact(zs)) {
        if (zvsetFitsCompact((unsigned long)zs->count + 1, zvItemMemberLen(item))) {
            zvCompactInsertAt(zs, zvCompactLowerBound(zs, item, sdslen(item)), item);
            return;
        }
        zvsetConvertToBtree(zs);
    }
    sds inserted = fbtreeInsert(zs->tree, item);
    serverAssert(hashtableAdd(zs->ht, inserted));
}

int zvsetTryInsert(zvset *zs, sds item) {
    if (zvsetFindItemMember(zs, item) != NULL) return 0;
    zvsetInsertNew(zs, item);
    return 1;
}

static int zvItemSdsCompare(const void *a, const void *b) {
    return sdscmp(*(const sds *)a, *(const sds *)b);
}

typedef struct zvSortEntry {
    uint64_t key; /* First score component, as an integer in item order. */
    sds item;
} zvSortEntry;

/* Sort items of one dimension count into item order. An LSD radix sort on
 * the first score component (whose sortable encoding orders like an
 * unsigned integer) touches each item once; only runs that tie on it are
 * compared in full. Much faster than qsort with sdscmp, which dereferences
 * two random items per comparison. */
static void zvSortItems(sds *items, unsigned long count) {
    if (count < 64) {
        qsort(items, count, sizeof(sds), zvItemSdsCompare);
        return;
    }
    zvSortEntry *a = zmalloc(sizeof(zvSortEntry) * count);
    zvSortEntry *b = zmalloc(sizeof(zvSortEntry) * count);
    for (unsigned long i = 0; i < count; i++) {
        uint64_t sortable;
        memcpy(&sortable, items[i] + 1, sizeof(sortable));
        a[i].key = ntohu64(sortable);
        a[i].item = items[i];
    }
    for (int shift = 0; shift < 64; shift += 8) {
        unsigned long counts[256] = {0};
        for (unsigned long i = 0; i < count; i++) counts[(a[i].key >> shift) & 0xff]++;
        /* Every key shares this byte: the pass would not move anything. */
        if (counts[(a[0].key >> shift) & 0xff] == count) continue;
        unsigned long pos = 0;
        for (int d = 0; d < 256; d++) {
            unsigned long n = counts[d];
            counts[d] = pos;
            pos += n;
        }
        for (unsigned long i = 0; i < count; i++) b[counts[(a[i].key >> shift) & 0xff]++] = a[i];
        zvSortEntry *t = a;
        a = b;
        b = t;
    }
    for (unsigned long i = 0; i < count; i++) items[i] = a[i].item;
    /* Order each run of equal first components by the full item. */
    for (unsigned long i = 0; i < count;) {
        unsigned long j = i + 1;
        while (j < count && a[j].key == a[i].key) j++;
        if (j - i > 1) qsort(items + i, j - i, sizeof(sds), zvItemSdsCompare);
        i = j;
    }
    zfree(a);
    zfree(b);
}

/* Build a set from count distinct items, taking ownership of them, in the
 * encoding that fits. items may be in any order and is sorted in place.
 * ht, when not NULL, indexes exactly these items and is reused. Inserting
 * in order keeps the tree build sequential instead of a random insert per
 * item. */
static zvset *zvsetBuild(uint8_t dimensions, sds *items, unsigned long count, hashtable *ht) {
    zvset *zs = zvsetCreate(dimensions);
    zvSortItems(items, count);
    int compact = zvsetFitsCompact(count, 0);
    for (unsigned long i = 0; compact && i < count; i++) {
        if (!zvsetFitsCompact(0, zvItemMemberLen(items[i]))) compact = 0;
    }
    if (compact) {
        if (ht) hashtableRelease(ht);
        zvCompactAdopt(zs, items, (uint32_t)count);
        return zs;
    }
    if (ht == NULL) {
        ht = hashtableCreate(&zvsetHashtableType);
        hashtableExpand(ht, count);
        for (unsigned long i = 0; i < count; i++) serverAssert(hashtableAdd(ht, items[i]));
    }
    zs->ht = ht;
    zs->tree = fbtreeCreate();
    for (unsigned long i = 0; i < count; i++) serverAssert(fbtreeInsert(zs->tree, items[i]) == items[i]);
    return zs;
}

zvset *zvsetFromItems(uint8_t dimensions, sds *items, unsigned long count) {
    return zvsetBuild(dimensions, items, count, NULL);
}

zvset *zvsetFromHashtable(uint8_t dimensions, hashtable *ht) {
    unsigned long len = hashtableSize(ht);
    sds *items = zmalloc(sizeof(sds) * (len ? len : 1));
    hashtableIterator iter;
    hashtableInitIterator(&iter, ht, 0);
    void *next;
    unsigned long i = 0;
    while (hashtableNext(&iter, &next)) items[i++] = next;
    hashtableCleanupIterator(&iter);
    zvset *zs = zvsetBuild(dimensions, items, len, ht);
    zfree(items);
    return zs;
}

void *zvsetFind(zvset *zs, sds member) {
    if (zvsetIsCompact(zs)) {
        long idx = zvCompactFindMember(zs, member, sdslen(member));
        return idx < 0 ? NULL : zs->items[idx];
    }
    void *entry = NULL;
    zsetMarkLookupKey(member);
    hashtableFind(zs->ht, member, &entry);
    zsetUnmarkLookupKey(member);
    return entry;
}

const_sds zvsetFindItemMember(zvset *zs, const_sds item) {
    if (zvsetIsCompact(zs)) {
        size_t len;
        const char *member = zvItemMember(item, &len);
        long idx = zvCompactFindMember(zs, member, len);
        return idx < 0 ? NULL : zs->items[idx];
    }
    /* Packed items hash and compare by their member. */
    void *found;
    return hashtableFind(zs->ht, item, &found) ? found : NULL;
}

long zvsetRankOf(zvset *zs, const_sds item) {
    if (zvsetIsCompact(zs)) {
        /* Comparing pointers in one contiguous array beats a binary search
         * that dereferences and compares whole items. */
        for (uint32_t i = 0; i < zs->count; i++)
            if (zs->items[i] == item) return (long)i;
        return -1;
    }
    return fbtreeGetIndexOfItem(zs->tree, item);
}

const_sds zvsetGetAtRank(zvset *zs, unsigned long rank) {
    if (zvsetIsCompact(zs)) return rank < zs->count ? zs->items[rank] : NULL;
    return fbtreeGetAtRank(zs->tree, rank);
}

static const_sds zvsetPeek(zvset *zs, int max) {
    if (zvsetIsCompact(zs)) {
        if (zs->count == 0) return NULL;
        return zs->items[max ? zs->count - 1 : 0];
    }
    return max ? fbtreePeekMax(zs->tree) : fbtreePeekMin(zs->tree);
}

sds zvsetPop(zvset *zs, int max) {
    if (zvsetIsCompact(zs)) {
        if (zs->count == 0) return NULL;
        unsigned long idx = max ? zs->count - 1 : 0;
        sds item = zs->items[idx];
        zvCompactRemoveAt(zs, idx, 1);
        return item;
    }
    sds item = max ? fbtreePopMax(zs->tree) : fbtreePopMin(zs->tree);
    /* Drop the hashtable reference while the item is still alive. */
    if (item) serverAssert(hashtableDelete(zs->ht, item));
    return item;
}

/* fbtree range deletion callback: drop the hashtable reference while the
 * item is still alive (fbtree frees it afterwards). */
static void zvRangeDeleteCallback(sds item, void *ctx) {
    zvset *zs = ctx;
    serverAssert(hashtableDelete(zs->ht, item));
}

unsigned long zvsetDeleteRangeByRank(zvset *zs, unsigned long lo, unsigned long hi) {
    unsigned long len = zvsetLength(zs);
    if (hi > len) hi = len;
    if (lo >= hi) return 0;
    if (zvsetIsCompact(zs)) {
        for (unsigned long i = lo; i < hi; i++) sdsfree(zs->items[i]);
        zvCompactRemoveAt(zs, lo, hi - lo);
        return hi - lo;
    }
    return fbtreeDeleteRangeByRank(zs->tree, lo, hi - 1, zvRangeDeleteCallback, zs);
}

void zvsetPauseAutoShrink(zvset *zs) {
    if (!zvsetIsCompact(zs)) hashtablePauseAutoShrink(zs->ht);
}

void zvsetResumeAutoShrink(zvset *zs) {
    if (!zvsetIsCompact(zs)) hashtableResumeAutoShrink(zs->ht);
}

size_t zvsetAllocSize(zvset *zs, size_t sample_size) {
    size_t asize = zmalloc_size(zs), elesize = 0, samples = 0;
    unsigned long len = zvsetLength(zs);
    if (zvsetIsCompact(zs)) {
        if (zs->items) asize += zmalloc_size(zs->items);
    } else {
        asize += fbtreeEstimateStructureMemory(zs->tree) + hashtableMemUsage(zs->ht);
    }
    zvsetIterator it;
    zvsetInitIterator(&it, zs);
    const_sds item;
    while (samples < sample_size && (item = zvsetNext(&it)) != NULL) {
        elesize += sdsAllocSize((sds)item);
        samples++;
    }
    if (samples) asize += (double)elesize / samples * len;
    return asize;
}

void zvsetInitIterator(zvsetIterator *it, zvset *zs) {
    it->zs = zs;
    it->pos = 0;
    it->fresh = 1;
    it->unordered = 0;
    if (!zvsetIsCompact(zs)) fbtreeInitIterator(&it->it, zs->tree);
}

void zvsetInitUnorderedIterator(zvsetIterator *it, zvset *zs) {
    zvsetInitIterator(it, zs);
    if (zvsetIsCompact(zs)) return;
    it->unordered = 1;
    hashtableInitIterator(&it->hit, zs->ht, 0);
}

void zvsetResetIterator(zvsetIterator *it) {
    if (it->unordered) hashtableCleanupIterator(&it->hit);
    it->unordered = 0;
}

void zvsetSeekToRank(zvsetIterator *it, unsigned long rank) {
    if (!zvsetIsCompact(it->zs)) {
        fbtreeSeekToRank(&it->it, rank);
        return;
    }
    it->pos = rank < it->zs->count ? rank : it->zs->count;
    it->fresh = 0;
}

const_sds zvsetNext(zvsetIterator *it) {
    if (it->unordered) {
        void *next;
        return hashtableNext(&it->hit, &next) ? next : NULL;
    }
    if (!zvsetIsCompact(it->zs)) return fbtreeNext(&it->it);
    it->fresh = 0;
    return it->pos < it->zs->count ? it->zs->items[it->pos++] : NULL;
}

const_sds zvsetPrev(zvsetIterator *it) {
    serverAssert(!it->unordered);
    if (!zvsetIsCompact(it->zs)) return fbtreePrev(&it->it);
    if (it->fresh) {
        it->pos = it->zs->count;
        it->fresh = 0;
    }
    return it->pos > 0 ? it->zs->items[--it->pos] : NULL;
}

void zvScoreRanks(zvset *zs, const_sds lower, const_sds upper, unsigned long *lo, unsigned long *hi) {
    unsigned long len = zvsetLength(zs);
    if (zvsetIsCompact(zs)) {
        *lo = lower ? zvCompactLowerBound(zs, lower, sdslen(lower)) : 0;
        *hi = upper ? zvCompactLowerBound(zs, upper, sdslen(upper)) : len;
    } else {
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
    }
    if (*lo > len) *lo = len;
    if (*hi > len) *hi = len;
    if (*lo > *hi) *lo = *hi;
}

void zvIterateRange(zvset *zs, unsigned long lo, unsigned long hi, int reverse, long offset, long count, zvRangeEmit emit, void *ctx) {
    if (lo >= hi) return;
    if (offset < 0) offset = 0;
    zvsetIterator it;
    zvsetInitIterator(&it, zs);
    if (!reverse) {
        unsigned long start = lo + (unsigned long)offset;
        if (start >= hi) return;
        unsigned long n = hi - start;
        if (count >= 0 && n > (unsigned long)count) n = (unsigned long)count;
        if (start > 0) zvsetSeekToRank(&it, start);
        for (unsigned long i = 0; i < n; i++) {
            const_sds item = zvsetNext(&it);
            if (item == NULL) break;
            emit(ctx, item);
        }
    } else {
        long start = (long)hi - 1 - offset;
        if (start < (long)lo) return;
        unsigned long n = (unsigned long)(start - (long)lo) + 1;
        if (count >= 0 && n > (unsigned long)count) n = (unsigned long)count;
        /* After SeekToRank(r), Prev() yields rank r-1. */
        zvsetSeekToRank(&it, (unsigned long)start + 1);
        for (unsigned long i = 0; i < n; i++) {
            const_sds item = zvsetPrev(&it);
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
    if (zvsetLength(zs) <= 1) return 1;
    const_sds first = zvsetPeek(zs, 0);
    const_sds last = zvsetPeek(zs, 1);
    serverAssert(first != NULL && last != NULL);
    if (zvItemPrefixCompare(first, last) != 0) return 0;
    return 1;
}

sds zvLexSeekKey(zvset *zs, const char *member, size_t member_len) {
    const_sds first = zvsetPeek(zs, 0);
    serverAssert(first != NULL);
    uint8_t dims = (uint8_t)first[0];
    size_t prefix = 1 + (size_t)dims * 8;
    serverAssert(sdslen(first) >= prefix);
    sds key = sdsnewlen(NULL, prefix + member_len);
    memcpy(key, first, prefix);
    memcpy(key + prefix, member, member_len);
    return key;
}

/* Stack storage for one BYSCORE seek key: an sds holding at most a full
 * encoded prefix, so resolving a range allocates nothing. */
#define ZV_SEEK_KEY_SIZE (sizeof(struct sdshdr16) + 1 + 8 * ZVSET_MAX_DIMENSIONS + 1)

/* Write the encoded prefix of values (or its successor) into storage as an
 * sds. Returns NULL when the successor does not exist. */
static sds zvSeekKey(char *storage, const double *values, uint8_t dims, int successor) {
    sds key = sdswrite(storage, ZV_SEEK_KEY_SIZE, SDS_TYPE_16, SDS_NOINIT, 1 + (size_t)dims * 8);
    size_t len = zvEncodePrefix(values, dims, key);
    if (successor && (len = zvSuccessorInPlace(key, len)) == 0) return NULL;
    sdssetlen(key, len);
    return key;
}

/* Build the BYSCORE seek keys for [minb,maxb] (already fitted to the
 * key's dimensions) in lower_buf and upper_buf (ZV_SEEK_KEY_SIZE bytes
 * each): items in range are those >= *lower and < *upper. NULL means
 * unbounded. Returns 0 when the range is trivially empty. */
static int zvBuildScoreInterval(const zvScoreBound *minb, const zvScoreBound *maxb, char *lower_buf, char *upper_buf, sds *lower, sds *upper) {
    *lower = NULL;
    *upper = NULL;
    /* Reversed infinities ("+" as min or "-" as max) are empty, never
     * a full range. */
    if (minb->pos_inf || maxb->neg_inf) return 0;
    /* Exclusive lower: skip every member with exactly this score. */
    if (minb->len && (*lower = zvSeekKey(lower_buf, minb->values, minb->len, minb->exclusive)) == NULL) return 0;
    /* Inclusive upper: end after every member with exactly this score. An
     * all-0xFF prefix has no successor and covers everything above. */
    if (maxb->len) *upper = zvSeekKey(upper_buf, maxb->values, maxb->len, !maxb->exclusive);
    return !(*lower && *upper && sdscmp(*lower, *upper) >= 0);
}

/* Build [lower,upper) seek keys for uniform-vector BYLEX. Member bounds
 * use a trailing NUL byte instead of a carry successor (which is only
 * valid for fixed-length score prefixes): exclusive lower "a" seeks
 * "a\0" (first strictly greater member, so "ab" is kept), while
 * inclusive upper "a" ends exclusively at "a\0" (so "ab" is excluded).
 * Returns C_OK; *empty is set when the range is trivially empty. The
 * key must be non-empty and uniform (checked by the caller). */
static int zvBuildLexInterval(zvset *zs, zvLexBound *minb, zvLexBound *maxb, sds *lower, sds *upper, int *empty) {
    *lower = NULL;
    *upper = NULL;
    *empty = 0;
    /* Reversed infinities ("+" as min or "-" as max) are empty, never
     * a full range. */
    if (minb->pos_inf || maxb->neg_inf) {
        *empty = 1;
        return C_OK;
    }
    if (!minb->unbounded && !maxb->unbounded) {
        int cmp = zvLexMemberCompare(minb->member, sdslen(minb->member), maxb->member, sdslen(maxb->member));
        if (cmp > 0 || (cmp == 0 && (minb->exclusive || maxb->exclusive))) {
            *empty = 1;
            return C_OK;
        }
    }
    if (!minb->unbounded) {
        sds key = zvLexSeekKey(zs, minb->member, sdslen(minb->member));
        if (minb->exclusive) {
            *lower = sdscatlen(key, "\0", 1);
        } else {
            *lower = key;
        }
    }
    if (!maxb->unbounded) {
        sds key = zvLexSeekKey(zs, maxb->member, sdslen(maxb->member));
        if (maxb->exclusive) {
            *upper = key;
        } else {
            *upper = sdscatlen(key, "\0", 1);
        }
    }
    return C_OK;
}

/* Resolve parsed BYSCORE bounds to half-open ranks [*lo,*hi). Returns
 * C_ERR when a bound does not fit the key's dimensions. */
int zvsetScoreRangeToRanks(zvset *zs, zvScoreBound *minb, zvScoreBound *maxb, unsigned long *lo, unsigned long *hi) {
    if (zvScoreBoundFit(minb, zs->dimensions) != C_OK || zvScoreBoundFit(maxb, zs->dimensions) != C_OK)
        return C_ERR;
    char lower_buf[ZV_SEEK_KEY_SIZE], upper_buf[ZV_SEEK_KEY_SIZE];
    sds lower, upper;
    *lo = *hi = 0;
    if (zvBuildScoreInterval(minb, maxb, lower_buf, upper_buf, &lower, &upper)) zvScoreRanks(zs, lower, upper, lo, hi);
    return C_OK;
}

/* Number of members in parsed BYSCORE bounds, or -1 when a bound does not
 * fit the key's dimensions. A bounded range is counted in one descent. */
long zvsetScoreRangeCount(zvset *zs, zvScoreBound *minb, zvScoreBound *maxb) {
    if (zvScoreBoundFit(minb, zs->dimensions) != C_OK || zvScoreBoundFit(maxb, zs->dimensions) != C_OK) return -1;
    char lower_buf[ZV_SEEK_KEY_SIZE], upper_buf[ZV_SEEK_KEY_SIZE];
    sds lower, upper;
    if (!zvBuildScoreInterval(minb, maxb, lower_buf, upper_buf, &lower, &upper)) return 0;
    if (lower && upper && !zvsetIsCompact(zs)) return (long)fbtreeCountRangeByValue(zs->tree, lower, upper, 0, 1);
    unsigned long lo, hi;
    zvScoreRanks(zs, lower, upper, &lo, &hi);
    return (long)(hi - lo);
}

/* Resolve a BYLEX [min,max] range to half-open ranks [*lo,*hi). The
 * caller must check zvsetUniformVector() first. Returns C_ERR when a
 * bound is malformed. */
int zvsetLexRangeToRanks(zvset *zs, const char *min, size_t minlen, const char *max, size_t maxlen, unsigned long *lo, unsigned long *hi) {
    *lo = *hi = 0;
    if (zvsetLength(zs) == 0) return C_OK;
    zvLexBound minb, maxb;
    if (zvParseLexBound(min, minlen, &minb) != C_OK) return C_ERR;
    if (zvParseLexBound(max, maxlen, &maxb) != C_OK) {
        zvFreeLexBound(&minb);
        return C_ERR;
    }
    sds lower, upper;
    int empty;
    zvBuildLexInterval(zs, &minb, &maxb, &lower, &upper, &empty);
    zvFreeLexBound(&minb);
    zvFreeLexBound(&maxb);
    if (!empty) zvScoreRanks(zs, lower, upper, lo, hi);
    sdsfree(lower);
    sdsfree(upper);
    return C_OK;
}

/* Rank a score would take if inserted: before every equal score
 * (forward) or after every equal score (reverse, counted from the top). */
unsigned long zvsetRankByScore(zvset *zs, const zvScore *score, int reverse) {
    char buf[ZV_SEEK_KEY_SIZE];
    unsigned long lo, hi;
    if (!reverse) {
        zvScoreRanks(zs, NULL, zvSeekKey(buf, score->values, score->len, 0), &lo, &hi);
        return hi;
    }
    /* An all-0xFF prefix has no successor: nothing ranks above it. */
    sds succ = zvSeekKey(buf, score->values, score->len, 1);
    if (succ == NULL) return 0;
    zvScoreRanks(zs, succ, NULL, &lo, &hi);
    return hi - lo;
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

/* Packed scores are canonical and byte ordered, so equality and GT/LT
 * need no decoded allocation. */
int zvItemScoreCompare(const_sds item, const zvScore *score) {
    serverAssert((uint8_t)item[0] == score->len);
    /* Encoding every component and doing one memcmp is cheaper than a
     * memcmp per component, even when an early component differs. */
    uint64_t encoded[ZVSET_MAX_DIMENSIONS];
    for (int i = 0; i < score->len; i++) encoded[i] = zvScoreToSortable(score->values[i]);
    return memcmp(item + 1, encoded, (size_t)score->len * sizeof(uint64_t));
}

/* The encoded form is canonical (-0 normalized at parse and pack time),
 * so equality is a byte comparison. */
int zvItemScoreEquals(const_sds item, const zvScore *score) {
    if ((uint8_t)item[0] != score->len) return 0;
    return zvItemScoreCompare(item, score) == 0;
}

/* Format a packed item's vector as "v0#v1#..." into buf (at least
 * ZVSET_SCORE_BUFLEN bytes), formatted like ZSET replies. */
size_t zvItemFormatScoreBuf(const_sds item, char *buf) {
    uint8_t dims = (uint8_t)item[0];
    size_t len = 0;
    for (int i = 0; i < dims; i++) {
        uint64_t sortable;
        memcpy(&sortable, item + 1 + (size_t)i * 8, sizeof(sortable));
        if (i > 0) buf[len++] = '#';
        len += zvFormatDouble(zvSortableToScore(sortable), buf + len);
    }
    return len;
}

sds zvItemFormatScore(const_sds item) {
    char buf[ZVSET_SCORE_BUFLEN];
    return sdsnewlen(buf, zvItemFormatScoreBuf(item, buf));
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

/* Position of a member found by zvsetLookup(): the hashtable slot for
 * btree, the array index for compact. */
typedef struct zvSlot {
    void **ref;
    long idx;
} zvSlot;

static const_sds zvsetLookup(zvset *zs, sds member, zvSlot *slot) {
    if (zvsetIsCompact(zs)) {
        slot->idx = zvCompactFindMember(zs, member, sdslen(member));
        return slot->idx < 0 ? NULL : zs->items[slot->idx];
    }
    zsetMarkLookupKey(member);
    slot->ref = hashtableFindRef(zs->ht, member);
    zsetUnmarkLookupKey(member);
    return slot->ref ? *slot->ref : NULL;
}

/* Replace the item at slot with one holding the same member and a new
 * score, keeping the order. The old item is freed. */
static void zvsetReplaceAt(zvset *zs, zvSlot *slot, const zvScore *score, sds member) {
    sds new_item = zvItemCreate(score, member, sdslen(member));
    if (zvsetIsCompact(zs)) {
        unsigned long i = (unsigned long)slot->idx;
        sds old_item = zs->items[i];
        uint16_t *tags = zvCompactTags(zs);
        uint16_t tag = tags[i];
        /* Move the slots between the old and new positions by one. */
        unsigned long j = zvCompactLowerBound(zs, new_item, sdslen(new_item));
        if (j > i) {
            j--;
            memmove(&zs->items[i], &zs->items[i + 1], sizeof(sds) * (j - i));
            memmove(&tags[i], &tags[i + 1], sizeof(uint16_t) * (j - i));
        } else {
            memmove(&zs->items[j + 1], &zs->items[j], sizeof(sds) * (i - j));
            memmove(&tags[j + 1], &tags[j], sizeof(uint16_t) * (i - j));
        }
        zs->items[j] = new_item;
        tags[j] = tag;
        sdsfree(old_item);
        return;
    }
    /* fbtree frees the old item; then point the slot at the new one. */
    serverAssert(fbtreeDelete(zs->tree, *slot->ref));
    *slot->ref = fbtreeInsert(zs->tree, new_item);
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

    zvSlot slot;
    const_sds old_item = zvsetLookup(zs, member, &slot);
    if (old_item != NULL) {
        if (nx) {
            *out_flags |= ZVADD_OUT_NOP;
            return C_OK;
        }
        if (zvItemScoreEquals(old_item, score)) return C_OK;

        /* GT/LT compare only through the first differing component. */
        if (gt || lt) {
            int cmp = zvItemScoreCompare(old_item, score);
            if ((gt && cmp >= 0) || (lt && cmp <= 0)) {
                *out_flags |= ZVADD_OUT_NOP;
                return C_OK;
            }
        }

        zvsetReplaceAt(zs, &slot, score, member);
        *out_flags |= ZVADD_OUT_UPDATED;
        return C_OK;
    }

    if (xx) {
        *out_flags |= ZVADD_OUT_NOP;
        return C_OK;
    }

    zvsetInsertNew(zs, zvItemCreate(score, member, sdslen(member)));
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

    zvSlot slot;
    const_sds old_item = zvsetLookup(zs, member, &slot);
    if (old_item != NULL) {
        if (nx) {
            *out_flags |= ZVADD_OUT_NOP;
            return C_OK;
        }
        zvsetItemToScore(old_item, newscore);
        if (zvScoreIncrement(newscore, newscore, delta) != C_OK) return C_ERR;
        /* cmp is the old score relative to the new one. */
        int cmp = zvItemScoreCompare(old_item, newscore);
        if ((gt && cmp >= 0) || (lt && cmp <= 0)) {
            *out_flags |= ZVADD_OUT_NOP;
            return C_OK;
        }
        /* Skip delete/insert when the result is identical. */
        if (cmp == 0) return C_OK;
        zvsetReplaceAt(zs, &slot, newscore, member);
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
    zvsetInsertNew(zs, zvItemCreate(newscore, member, sdslen(member)));
    *out_flags |= ZVADD_OUT_ADDED;
    return C_OK;
}

int zvsetDel(zvset *zs, sds member) {
    if (zvsetIsCompact(zs)) {
        long idx = zvCompactFindMember(zs, member, sdslen(member));
        if (idx < 0) return 0;
        sdsfree(zs->items[idx]);
        zvCompactRemoveAt(zs, (unsigned long)idx, 1);
        return 1;
    }
    void *item = NULL;
    zsetMarkLookupKey(member);
    int found = hashtablePop(zs->ht, member, &item);
    zsetUnmarkLookupKey(member);
    if (!found) return 0;
    /* hashtable holds a non-owning pointer; fbtree owns and frees it. */
    serverAssert(fbtreeDelete(zs->tree, item));
    return 1;
}

/* Duplicate a zvset object (for COPY). Same encoding guarantee. */
robj *zvsetDup(robj *o) {
    serverAssert(objectGetType(o) == OBJ_ZVSET);
    zvset *zs = objectGetVal(o);
    robj *zobj = createZvsetObject(zs->dimensions);
    zvset *new_zs = objectGetVal(zobj);
    if (zvsetIsCompact(zs)) {
        if (zs->count) {
            zvCompactResize(new_zs, zs->count);
            uint16_t *tags = zvCompactTags(zs), *new_tags = zvCompactTags(new_zs);
            for (uint32_t i = 0; i < zs->count; i++) {
                new_zs->items[i] = sdsdup(zs->items[i]);
                new_tags[i] = tags[i];
            }
            new_zs->count = zs->count;
        }
        return zobj;
    }
    zvsetConvertToBtree(new_zs);
    hashtableExpand(new_zs->ht, hashtableSize(zs->ht));
    /* Ascending inserts take fbtree's append path and leave full leaves. */
    fbtreeIterator iter;
    fbtreeInitIterator(&iter, zs->tree);
    const_sds item;
    while ((item = fbtreeNext(&iter)) != NULL) {
        sds copy = sdsdup(item);
        sds inserted = fbtreeInsert(new_zs->tree, copy);
        serverAssert(hashtableAdd(new_zs->ht, inserted));
    }
    return zobj;
}
