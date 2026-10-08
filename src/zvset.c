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

int zvsetAdd(zvset *zs, const zvScore *score, sds member, int in_flags, int *out_flags) {
    int nx = in_flags & ZVADD_IN_NX;
    int xx = in_flags & ZVADD_IN_XX;

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

        sds new_item = zvItemCreate(score, member, sdslen(member));
        serverAssert(fbtreeDelete(zs->tree, old_item));
        sds inserted = fbtreeInsert(zs->tree, new_item);
        *item_ref = inserted;
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
