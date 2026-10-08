#ifndef ZVSET_H
#define ZVSET_H

#include "sds.h"
#include "hashtable.h"
#include <stddef.h>
#include <stdint.h>

/* Forward declarations. */
typedef struct fbtreeIndex fbtreeIndex;
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
 */
typedef struct zvset {
    hashtable *ht;
    fbtreeIndex *tree;
    uint8_t dimensions;
} zvset;

#define ZVSET_MAX_DIMENSIONS 255

/* ZVADD input flags. */
#define ZVADD_IN_NONE 0
#define ZVADD_IN_NX (1 << 0)
#define ZVADD_IN_XX (1 << 1)

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
void zvScoreFree(zvScore *score);

/* double <-> sortable conversion (same rule as ordered_index.c). */
uint64_t zvScoreToSortable(double score);
double zvSortableToScore(uint64_t sortable);

/* Packed item helpers. */
sds zvItemCreate(const zvScore *score, const char *member, size_t member_len);
uint8_t zvItemDimensions(const_sds item);
const char *zvItemMember(const_sds item, size_t *member_len);
double zvItemScoreAt(const_sds item, uint8_t dimension);
int zvItemScoreEquals(const_sds item, const zvScore *score);
/* Format vector score as "v0#v1#..." (Tair style). Caller must sdsfree(). */
sds zvItemFormatScore(const_sds item);

/* Core operations. member is a plain SDS (not owned). */
int zvsetAdd(zvset *zs, const zvScore *score, sds member, int in_flags, int *out_flags);
int zvsetDel(zvset *zs, sds member);
/* O(1) hashtable lookup. Returns packed item pointer or NULL. */
void *zvsetFind(zvset *zs, sds member);
unsigned long zvsetLength(const zvset *zs);
struct serverObject *zvsetDup(struct serverObject *o);

#endif
