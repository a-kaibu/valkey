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
        if (i == start || count == ZVSET_MAX_DIMENSIONS || !string2d(str + start, i - start, &v) || isnan(v))
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

/* Build the BYSCORE seek keys for [minb,maxb] (already fitted to the
 * key's dimensions): items in range are those >= *lower and < *upper.
 * NULL means unbounded. Returns 0 when the range is trivially empty.
 * Caller frees *lower and *upper. */
static int zvBuildScoreInterval(const zvScoreBound *minb, const zvScoreBound *maxb, sds *lower, sds *upper) {
    *lower = NULL;
    *upper = NULL;
    /* Reversed infinities ("+" as min or "-" as max) are empty, never
     * a full range. */
    if (minb->pos_inf || maxb->neg_inf) return 0;
    char buf[1 + 8 * ZVSET_MAX_DIMENSIONS];
    if (minb->len) {
        size_t len = zvEncodePrefix(minb->values, minb->len, buf);
        /* Exclusive: skip every member with exactly this score. */
        if (minb->exclusive && (len = zvSuccessorInPlace(buf, len)) == 0) return 0;
        *lower = sdsnewlen(buf, len);
    }
    if (maxb->len) {
        size_t len = zvEncodePrefix(maxb->values, maxb->len, buf);
        /* Inclusive: end after every member with exactly this score. An
         * all-0xFF prefix has no successor and covers everything above. */
        if (!maxb->exclusive) len = zvSuccessorInPlace(buf, len);
        if (len) *upper = sdsnewlen(buf, len);
    }
    if (*lower && *upper && sdscmp(*lower, *upper) >= 0) {
        sdsfree(*lower);
        sdsfree(*upper);
        *lower = *upper = NULL;
        return 0;
    }
    return 1;
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
    sds lower, upper;
    *lo = *hi = 0;
    if (zvBuildScoreInterval(minb, maxb, &lower, &upper)) zvScoreRanks(zs, lower, upper, lo, hi);
    sdsfree(lower);
    sdsfree(upper);
    return C_OK;
}

/* Number of members in parsed BYSCORE bounds, or -1 when a bound does not
 * fit the key's dimensions. A bounded range is counted in one descent. */
long zvsetScoreRangeCount(zvset *zs, zvScoreBound *minb, zvScoreBound *maxb) {
    if (zvScoreBoundFit(minb, zs->dimensions) != C_OK || zvScoreBoundFit(maxb, zs->dimensions) != C_OK) return -1;
    sds lower, upper;
    unsigned long count = 0;
    if (zvBuildScoreInterval(minb, maxb, &lower, &upper)) {
        if (lower && upper) {
            count = fbtreeCountRangeByValue(zs->tree, lower, upper, 0, 1);
        } else {
            unsigned long lo, hi;
            zvScoreRanks(zs, lower, upper, &lo, &hi);
            count = hi - lo;
        }
    }
    sdsfree(lower);
    sdsfree(upper);
    return (long)count;
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
    char buf[1 + 8 * ZVSET_MAX_DIMENSIONS];
    size_t len = zvEncodePrefix(score->values, score->len, buf);
    unsigned long lo, hi;
    if (!reverse) {
        sds prefix = sdsnewlen(buf, len);
        zvScoreRanks(zs, NULL, prefix, &lo, &hi);
        sdsfree(prefix);
        return hi;
    }
    /* An all-0xFF prefix has no successor: nothing ranks above it. */
    if ((len = zvSuccessorInPlace(buf, len)) == 0) return 0;
    sds succ = sdsnewlen(buf, len);
    zvScoreRanks(zs, succ, NULL, &lo, &hi);
    sdsfree(succ);
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

        /* GT/LT compare only through the first differing component. */
        if (gt || lt) {
            int cmp = zvItemScoreCompare(old_item, score);
            if ((gt && cmp >= 0) || (lt && cmp <= 0)) {
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
        zvsetItemToScore(*item_ref, newscore);
        if (zvScoreIncrement(newscore, newscore, delta) != C_OK) return C_ERR;
        /* cmp is the old score relative to the new one. */
        int cmp = zvItemScoreCompare(*item_ref, newscore);
        if ((gt && cmp >= 0) || (lt && cmp <= 0)) {
            *out_flags |= ZVADD_OUT_NOP;
            return C_OK;
        }
        /* Skip delete/insert when the result is identical. */
        if (cmp == 0) return C_OK;
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
