#ifndef ZVSET_H
#define ZVSET_H

#include "sds.h"
#include "hashtable.h"
#include "fbtree.h"
#include <stddef.h>
#include <stdint.h>

/* Forward declarations. */
struct client;
struct serverObject;

/* ZVSET: native multi-score sorted set (PoC).
 *
 * Packed fbtree item layout (single SDS):
 *   +------+----------+----------+-----+----------+--------+
 *   | dims | score[0] | score[1] | ... | score[n] | member |
 *   +------+----------+----------+-----+----------+--------+
 *     1B       8B         8B              8B      variable
 *
 * Each score is stored as sortable big-endian u64 so that byte
 * lexicographical comparison of packed items matches numeric vector
 * lexicographical ordering. dims is identical for all items of a key,
 * so the leading byte does not affect ordering within a key.
 *
 * Two encodings, both holding the same packed items:
 *
 * - compact: a sorted array of items (no hashtable, no tree). Member
 *   lookups scan the array, rank lookups binary search it. Used while the
 *   key has at most zset-max-listpack-entries members, each at most
 *   zset-max-listpack-value bytes, like ZSET's listpack encoding.
 * - btree: fbtree (owns the items, ordered) plus a hashtable of
 *   non-owning pointers to them (member lookups).
 *
 * A key converts to btree when it outgrows the compact limits and never
 * converts back in place; newly built results (STORE commands) start in
 * whichever encoding fits their size.
 */
typedef struct zvset {
    hashtable *ht;     /* btree: member -> item (non-owning) */
    fbtreeIndex *tree; /* btree: ordered items (owning); NULL = compact */
    sds *items;        /* compact: ordered items (owning), then a member
                        * hash tag per slot (see zvCompactTags()) */
    uint32_t count;    /* compact: number of items */
    uint32_t capacity; /* compact: allocated slots */
    uint8_t dimensions;
} zvset;

#define ZVSET_MAX_DIMENSIONS 255
/* Formatted "v0#v1#..." upper bound: fpconv_dtoa writes at most 24 bytes
 * per component, plus one '#' separator. */
#define ZVSET_SCORE_BUFLEN (ZVSET_MAX_DIMENSIONS * 25)

/* ZVADD input flags. */
#define ZVADD_IN_NONE 0
#define ZVADD_IN_NX (1 << 0)
#define ZVADD_IN_XX (1 << 1)
#define ZVADD_IN_GT (1 << 2)
#define ZVADD_IN_LT (1 << 3)
#define ZVADD_IN_INCR (1 << 4)

/* ZVADD output flags. */
#define ZVADD_OUT_ADDED (1 << 0)
#define ZVADD_OUT_UPDATED (1 << 1)
#define ZVADD_OUT_NOP (1 << 2)

/* Temporary parsed score vector. Allocated with zvScoreParse(),
 * released with zvScoreFree(). */
typedef struct zvScore {
    uint8_t len;
    double values[];
} zvScore;

/* Hashtable type for zvset (entries are packed fbtree items). */
extern hashtableType zvsetHashtableType;

/* Score vector parsing ("1#2#3.5" style, '#' separated). */
zvScore *zvScoreParse(const char *str, size_t len);
zvScore *zvScoreCreate(uint8_t len);
void zvScoreFree(zvScore *score);
/* Lexicographic compare of two same-length vectors: -1/0/1. */
int zvScoreCompare(const zvScore *a, const zvScore *b);
/* result = base + delta per component. Returns C_OK, or C_ERR if any
 * component is NaN (result untouched beyond the failing index). */
int zvScoreIncrement(zvScore *result, const zvScore *base, const zvScore *delta);
/* Format a parsed vector as "v0#v1#...". Caller must sdsfree(). */
sds zvScoreFormat(const zvScore *score);
/* Same, into buf of ZVSET_SCORE_BUFLEN bytes. Returns the length. */
size_t zvScoreFormatBuf(const zvScore *score, char *buf);
/* Decode a packed item's full vector into out (len must match). */
void zvItemToScore(const_sds item, zvScore *out);

/* Set-operation aggregation modes. */
#define ZV_AGGR_SUM 0
#define ZV_AGGR_MIN 1
#define ZV_AGGR_MAX 2

/* Aggregate weighted into acc (same length). SUM adds component-wise;
 * MIN/MAX pick the lexicographically smaller/larger whole vector.
 * NaN components are normalized to 0 (setops never store NaN). */
void zvScoreAggregate(zvScore *acc, const zvScore *weighted, int aggregate);
/* weight*v per component with NaN->0 and -0 normalization. */
void zvScoreApplyWeight(zvScore *out, const zvScore *v, double weight);

/* BYSCORE bound: "-", "+", "(v0#v1" (exclusive) or "v0#v1" (inclusive).
 * A bare "-" is only valid as the lower bound (unbounded below) and a
 * bare "+" only as the upper bound (unbounded above); the reverse means
 * an empty range (never a full-range delete). The token side is recorded
 * so reversed infinities stay empty instead of collapsing to unbounded.
 * Parsed in place without allocation; len is 0 for "-" and "+". */
typedef struct zvScoreBound {
    int exclusive;
    int neg_inf; /* "-" token */
    int pos_inf; /* "+" token */
    uint8_t len;
    double values[ZVSET_MAX_DIMENSIONS];
} zvScoreBound;

/* Parse one bound token (syntax only; dimensions are checked against the
 * key when the range is resolved). */
int zvParseScoreBound(const char *str, size_t len, zvScoreBound *bound);
/* Parse "v0#v1#..." into out (ZVSET_MAX_DIMENSIONS values). Returns the
 * number of components, or 0 on error. */
int zvScoreParseInto(const char *str, size_t len, double *out);

/* Byte successor with carry (encoded-prefix space only, never decoded).
 * Returns NULL when the input is all 0xFF (unreachable for encoded
 * scores, since NaN is rejected). */
sds zvBoundSuccessor(const_sds bound);

/* Resolve half-open [lo,hi) ranks for [lower,upper); NULL = unbounded. */
void zvScoreRanks(zvset *zs, const_sds lower, const_sds upper, unsigned long *lo, unsigned long *hi);

/* BYLEX bound: "-", "+", "[member" (inclusive) or "(member" (exclusive).
 * Directional infinities behave like score bounds: "+" as the lower
 * bound or "-" as the upper bound means an empty range. */
typedef struct zvLexBound {
    int unbounded;
    int exclusive;
    int neg_inf;
    int pos_inf;
    sds member; /* NULL when unbounded */
} zvLexBound;

int zvParseLexBound(const char *str, size_t len, zvLexBound *bound);
void zvFreeLexBound(zvLexBound *bound);
/* sdscmp-style member comparison. */
int zvLexMemberCompare(const char *a, size_t alen, const char *b, size_t blen);
/* 1 when the key holds 0/1 items or every item shares the same score
 * prefix (then tree order equals member order). */
int zvsetUniformVector(zvset *zs);
/* Build a seek key P+member from the shared score prefix. The key must
 * be non-empty and uniform. Caller frees the result. */
sds zvLexSeekKey(zvset *zs, const char *member, size_t member_len);

/* Resolve protocol range bounds to half-open ranks [lo,hi). Both return
 * C_ERR on a malformed bound (or, for scores, a dimension mismatch).
 * BYLEX additionally requires zvsetUniformVector(). */
int zvsetScoreRangeToRanks(zvset *zs, zvScoreBound *minb, zvScoreBound *maxb, unsigned long *lo, unsigned long *hi);
/* Member count in [minb,maxb], or -1 when a bound does not fit the key. */
long zvsetScoreRangeCount(zvset *zs, zvScoreBound *minb, zvScoreBound *maxb);
int zvsetLexRangeToRanks(zvset *zs, const char *min, size_t minlen, const char *max, size_t maxlen, unsigned long *lo, unsigned long *hi);
/* Rank score would take on insertion (ZVRANKBYSCORE/ZVREVRANKBYSCORE). */
unsigned long zvsetRankByScore(zvset *zs, const zvScore *score, int reverse);

/* Range iteration over [lo,hi): reverse selects direction, offset/count
 * apply LIMIT (count < 0 means all). emit is called per item. */
typedef void (*zvRangeEmit)(void *ctx, const_sds item);
void zvIterateRange(zvset *zs, unsigned long lo, unsigned long hi, int reverse, long offset, long count, zvRangeEmit emit, void *ctx);

/* double <-> sortable conversion (same rule as ordered_index.c). */
uint64_t zvScoreToSortable(double score);
double zvSortableToScore(uint64_t sortable);

/* Packed item helpers. */
sds zvItemCreate(const zvScore *score, const char *member, size_t member_len);
uint8_t zvItemDimensions(const_sds item);
const char *zvItemMember(const_sds item, size_t *member_len);
double zvItemScoreAt(const_sds item, uint8_t dimension);
int zvItemScoreEquals(const_sds item, const zvScore *score);
/* Sign of item's vector minus score (same dimensions). */
int zvItemScoreCompare(const_sds item, const zvScore *score);
/* Format vector score as "v0#v1#..." (Tair style). Caller must sdsfree(). */
sds zvItemFormatScore(const_sds item);
/* Same, into buf of ZVSET_SCORE_BUFLEN bytes. Returns the length. */
size_t zvItemFormatScoreBuf(const_sds item, char *buf);

/* --- Encoding-independent container API --- */

zvset *zvsetCreate(uint8_t dimensions);
/* Create in the encoding that fits size_hint members. */
zvset *zvsetCreateSized(uint8_t dimensions, unsigned long size_hint);
void zvsetRelease(zvset *zs);
static inline int zvsetIsCompact(const zvset *zs) {
    return zs->tree == NULL;
}
const char *zvsetEncodingName(const zvset *zs);
/* Convert a btree key that fits the compact limits back to compact. */
void zvsetCompactIfFits(zvset *zs);
/* Reserve room for size members; returns 0 on allocation failure. */
int zvsetTryExpand(zvset *zs, unsigned long size);
/* Insert an item (ownership transfers) whose member is absent. */
void zvsetInsertNew(zvset *zs, sds item);
/* Same, but returns 0 without taking ownership if the member exists. */
int zvsetTryInsert(zvset *zs, sds item);
/* Build a zvset from a hashtable of owned items (zvsetHashtableType).
 * The hashtable is consumed. */
zvset *zvsetFromHashtable(uint8_t dimensions, hashtable *ht);
/* The stored item whose member equals item's member, or NULL. */
const_sds zvsetFindItemMember(zvset *zs, const_sds item);
long zvsetRankOf(zvset *zs, const_sds item);
const_sds zvsetGetAtRank(zvset *zs, unsigned long rank);
/* Remove and return the min/max item; the caller frees it. */
sds zvsetPop(zvset *zs, int max);
/* Delete (and free) the items at ranks [lo,hi). Returns the count. */
unsigned long zvsetDeleteRangeByRank(zvset *zs, unsigned long lo, unsigned long hi);
/* Batch deletes: avoid hashtable shrinking until resumed. */
void zvsetPauseAutoShrink(zvset *zs);
void zvsetResumeAutoShrink(zvset *zs);
size_t zvsetAllocSize(zvset *zs, size_t sample_size);

/* Ordered iterator. After init, Next() starts at rank 0 and Prev() at
 * the last rank; after SeekToRank(r), Next() yields r and Prev() r-1.
 * The set must not be modified while iterating. */
typedef struct zvsetIterator {
    zvset *zs;
    unsigned long pos; /* compact: rank Next() returns */
    int fresh;         /* compact: Prev() starts from the end */
    fbtreeIterator it;
} zvsetIterator;

void zvsetInitIterator(zvsetIterator *it, zvset *zs);
void zvsetSeekToRank(zvsetIterator *it, unsigned long rank);
const_sds zvsetNext(zvsetIterator *it);
const_sds zvsetPrev(zvsetIterator *it);

/* Core operations. member is a plain SDS (not owned). */
int zvsetAdd(zvset *zs, const zvScore *score, sds member, int in_flags, int *out_flags);
/* Increment member's vector by delta (missing member starts at zero).
 * newscore (len == dims) receives the result on success. Returns C_OK,
 * or C_ERR on NaN result (no mutation). Honors NX/XX/GT/LT. */
int zvsetIncrBy(zvset *zs, const zvScore *delta, sds member, int in_flags, int *out_flags, zvScore *newscore);
int zvsetDel(zvset *zs, sds member);
/* O(1) hashtable lookup. Returns packed item pointer or NULL. */
void *zvsetFind(zvset *zs, sds member);
unsigned long zvsetLength(const zvset *zs);
struct serverObject *zvsetDup(struct serverObject *o);

#endif
