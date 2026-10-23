/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "server.h"
#include "zvset.h"
#include "sds.h"
#include "hashtable.h"
#include "fbtree.h"
#include "mt19937-64.h"
#include "rio.h"
#include "zmalloc.h"

#include <strings.h>

/* Reply with a packed item's vector score without a temporary SDS. */
static void addReplyZvScore(client *c, const_sds item) {
    char buf[ZVSET_SCORE_BUFLEN];
    addReplyBulkCBuffer(c, buf, zvItemFormatScoreBuf(item, buf));
}

/* Reply with one range element: member, plus the score with WITHSCORES.
 * RESP3 nests [member, score] pairs like ZRANGE; RESP2 stays flat. */
static void addReplyZvElement(client *c, const_sds item, int withscores) {
    size_t member_len;
    const char *member = zvItemMember(item, &member_len);
    if (withscores && c->resp > 2) addReplyArrayLen(c, 2);
    addReplyBulkCBuffer(c, member, member_len);
    if (withscores) addReplyZvScore(c, item);
}

/* ZVADD key [NX|XX] [GT|LT] [CH] [INCR] score member [score member ...]
 *
 * score is "v0#v1#...#vn" (Tair style). The storage representation is
 * the packed fbtree item; the '#' syntax is only the protocol
 * representation parsed by zvScoreParse(). */
static void zvaddGenericCommand(client *c, int base_flags) {
    robj *key = c->argv[1];
    robj *zobj;
    int in_flags = base_flags;
    int ch = 0;
    int scoreidx = 2;
    int elements;
    int j;

    /* Parse optional flags. Accept NX/XX/GT/LT/CH/INCR in any order.
     * ZVINCRBY (base INCR) takes no options. */
    while (!(base_flags & ZVADD_IN_INCR) && scoreidx < c->argc) {
        char *opt = objectGetVal(c->argv[scoreidx]);
        if (!strcasecmp(opt, "nx")) {
            in_flags |= ZVADD_IN_NX;
        } else if (!strcasecmp(opt, "xx")) {
            in_flags |= ZVADD_IN_XX;
        } else if (!strcasecmp(opt, "gt")) {
            in_flags |= ZVADD_IN_GT;
        } else if (!strcasecmp(opt, "lt")) {
            in_flags |= ZVADD_IN_LT;
        } else if (!strcasecmp(opt, "ch")) {
            ch = 1;
        } else if (!strcasecmp(opt, "incr")) {
            in_flags |= ZVADD_IN_INCR;
        } else {
            break;
        }
        scoreidx++;
    }

    int incr = (in_flags & ZVADD_IN_INCR) != 0;

    elements = c->argc - scoreidx;
    if (elements % 2 || elements <= 0) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }
    elements /= 2;

    if ((in_flags & ZVADD_IN_NX) && (in_flags & ZVADD_IN_XX)) {
        addReplyError(c, "XX and NX options at the same time are not compatible");
        return;
    }
    if ((in_flags & ZVADD_IN_GT) && (in_flags & ZVADD_IN_LT)) {
        addReplyError(c, "GT and LT options at the same time are not compatible");
        return;
    }
    if (((in_flags & ZVADD_IN_GT) || (in_flags & ZVADD_IN_LT)) && (in_flags & ZVADD_IN_NX)) {
        addReplyError(c, "GT, LT, and/or NX options at the same time are not compatible");
        return;
    }
    if (incr && elements > 1) {
        addReplyError(c, "INCR option supports a single increment-element pair");
        return;
    }

    /* Parse all scores before mutating (atomicity: either all or nothing). */
    serverAssert(elements > 0);
    zvScore **scores = zmalloc(sizeof(*scores) * (size_t)(unsigned int)elements);
    for (j = 0; j < elements; j++) {
        scores[j] = NULL;
    }
    uint8_t dims = 0;
    for (j = 0; j < elements; j++) {
        sds scorestr = objectGetVal(c->argv[scoreidx + j * 2]);
        zvScore *s = zvScoreParse(scorestr, sdslen(scorestr));
        if (s == NULL) {
            for (int k = 0; k < j; k++) zvScoreFree(scores[k]);
            zfree(scores);
            addReplyError(c, "invalid vector score value, must be like '1#2#3.5'");
            return;
        }
        if (j == 0) {
            dims = s->len;
        } else if (s->len != dims) {
            zvScoreFree(s);
            for (int k = 0; k < j; k++) zvScoreFree(scores[k]);
            zfree(scores);
            addReplyError(c, "all vector scores must have the same dimension");
            return;
        }
        scores[j] = s;
    }

    zobj = lookupKeyWrite(c->db, key);
    if (checkType(c, zobj, OBJ_ZVSET)) {
        for (j = 0; j < elements; j++) zvScoreFree(scores[j]);
        zfree(scores);
        return;
    }
    if (zobj == NULL) {
        if (in_flags & ZVADD_IN_XX) {
            /* No key + XX: nothing to do. */
            for (j = 0; j < elements; j++) zvScoreFree(scores[j]);
            zfree(scores);
            if (incr)
                addReplyNull(c);
            else
                addReplyLongLong(c, 0);
            return;
        }
        zobj = createZvsetObject(dims);
        dbAdd(c->db, key, &zobj);
    } else {
        zvset *zs = objectGetVal(zobj);
        if (zs->dimensions != dims) {
            for (j = 0; j < elements; j++) zvScoreFree(scores[j]);
            zfree(scores);
            addReplyError(c, "vector dimension mismatch for existing key");
            return;
        }
    }

    long added = 0, updated = 0;
    int processed = 0;
    int reply_err = 0;
    zvScore *incr_result = NULL;
    if (incr) incr_result = zvScoreCreate(dims);
    for (j = 0; j < elements; j++) {
        int out_flags = 0;
        sds member = objectGetVal(c->argv[scoreidx + 1 + j * 2]);
        if (incr) {
            int ret = zvsetIncrBy(objectGetVal(zobj), scores[j], member, in_flags, &out_flags, incr_result);
            if (ret != C_OK) {
                reply_err = 1;
                break;
            }
        } else {
            int ret = zvsetAdd(objectGetVal(zobj), scores[j], member, in_flags, &out_flags);
            serverAssert(ret == C_OK);
        }
        if (out_flags & ZVADD_OUT_ADDED) added++;
        if (out_flags & ZVADD_OUT_UPDATED) updated++;
        if (!(out_flags & ZVADD_OUT_NOP)) processed++;
    }
    for (j = 0; j < elements; j++) zvScoreFree(scores[j]);
    zfree(scores);

    if (added || updated) {
        signalModifiedKey(c, c->db, key);
        notifyKeyspaceEvent(NOTIFY_ZSET, incr ? "zvincrby" : "zvadd", key, c->db->id);
        server.dirty += (added + updated);
    }
    if (reply_err) {
        if (incr_result) zvScoreFree(incr_result);
        addReplyError(c, "increment would produce NaN");
    } else if (incr) {
        if (processed) {
            char buf[ZVSET_SCORE_BUFLEN];
            addReplyBulkCBuffer(c, buf, zvScoreFormatBuf(incr_result, buf));
        } else {
            addReplyNull(c);
        }
        zvScoreFree(incr_result);
    } else {
        addReplyLongLong(c, ch ? added + updated : added);
    }
}

void zvaddCommand(client *c) {
    zvaddGenericCommand(c, ZVADD_IN_NONE);
}

void zvincrbyCommand(client *c) {
    /* ZVINCRBY key increment-vector member */
    if (c->argc != 4) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }
    /* Reuse the generic path: key INCR score member. */
    zvaddGenericCommand(c, ZVADD_IN_INCR);
}

void zvremCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;
    int deleted = 0, keyremoved = 0, j;

    if ((zobj = lookupKeyWriteOrReply(c, key, shared.czero)) == NULL || checkType(c, zobj, OBJ_ZVSET))
        return;

    zvset *zs = objectGetVal(zobj);
    hashtablePauseAutoShrink(zs->ht);
    for (j = 2; j < c->argc; j++) {
        if (zvsetDel(zs, objectGetVal(c->argv[j]))) deleted++;
        if (zvsetLength(zs) == 0) {
            /* dbDelete() frees zobj (and zs/ht with it); do not touch
             * zs afterwards. */
            dbDelete(c->db, key);
            keyremoved = 1;
            break;
        }
    }
    if (!keyremoved) hashtableResumeAutoShrink(zs->ht);

    if (deleted) {
        signalModifiedKey(c, c->db, key);
        notifyKeyspaceEvent(NOTIFY_ZSET, "zvrem", key, c->db->id);
        if (keyremoved) notifyKeyspaceEvent(NOTIFY_GENERIC, "del", key, c->db->id);
        server.dirty += deleted;
    }
    addReplyLongLong(c, deleted);
}

void zvscoreCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;

    if ((zobj = lookupKeyReadOrReply(c, key, shared.null[c->resp])) == NULL || checkType(c, zobj, OBJ_ZVSET))
        return;

    zvset *zs = objectGetVal(zobj);
    void *entry = zvsetFind(zs, objectGetVal(c->argv[2]));
    if (entry == NULL) {
        addReplyNull(c);
        return;
    }
    addReplyZvScore(c, entry);
}

void zvmscoreCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;

    zobj = lookupKeyRead(c->db, key);
    if (zobj == NULL) {
        addReplyArrayLen(c, c->argc - 2);
        for (int j = 2; j < c->argc; j++) {
            addReplyNull(c);
        }
        return;
    }
    if (checkType(c, zobj, OBJ_ZVSET)) return;

    zvset *zs = objectGetVal(zobj);
    addReplyArrayLen(c, c->argc - 2);
    for (int j = 2; j < c->argc; j++) {
        void *entry = zvsetFind(zs, objectGetVal(c->argv[j]));
        if (entry == NULL) {
            addReplyNull(c);
            continue;
        }
        addReplyZvScore(c, entry);
    }
}

/* Shared RANK implementation. reverse=0 → ZVRANK, 1 → ZVREVRANK. */
static void zvrankGenericCommand(client *c, int reverse) {
    robj *key = c->argv[1];
    robj *zobj;
    int withscore = 0;

    if (c->argc < 3 || c->argc > 4) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }
    if (c->argc == 4) {
        if (strcasecmp(objectGetVal(c->argv[3]), "withscore")) {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
        withscore = 1;
    }

    if ((zobj = lookupKeyReadOrReply(c, key, shared.null[c->resp])) == NULL || checkType(c, zobj, OBJ_ZVSET))
        return;

    zvset *zs = objectGetVal(zobj);
    void *entry = zvsetFind(zs, objectGetVal(c->argv[2]));
    if (entry == NULL) {
        addReplyNull(c);
        return;
    }
    long rank = fbtreeGetIndexOfItem(zs->tree, entry);
    serverAssert(rank >= 0);
    if (reverse) rank = (long)zvsetLength(zs) - 1 - rank;
    if (!withscore) {
        addReplyLongLong(c, rank);
        return;
    }
    addReplyArrayLen(c, 2);
    addReplyLongLong(c, rank);
    addReplyZvScore(c, entry);
}

void zvrankCommand(client *c) {
    zvrankGenericCommand(c, 0);
}

void zvrevrankCommand(client *c) {
    zvrankGenericCommand(c, 1);
}

typedef enum {
    ZV_RANGE_RANK,
    ZV_RANGE_SCORE,
    ZV_RANGE_LEX,
} zvRangeType;

/* A parsed range request. min/max are the raw bound tokens (already in
 * min/max order, i.e. swapped for REV) for SCORE/LEX, and start/stop are
 * the indexes for RANK. */
typedef struct zvRangeSpec {
    zvRangeType type;
    int reverse;
    int withscores;
    long start, stop;
    sds min, max;
    long offset; /* LIMIT offset, 0 without LIMIT. */
    long count;  /* LIMIT count, -1 (all) without LIMIT. */
} zvRangeSpec;

/* Option groups accepted by zvParseRangeOptions(). */
#define ZV_OPT_BY (1 << 0)
#define ZV_OPT_REV (1 << 1)
#define ZV_OPT_WITHSCORES (1 << 2)
#define ZV_OPT_LIMIT (1 << 3)

/* Parse c->argv[first..argc) as range options restricted to 'allowed'.
 * Returns C_OK, or C_ERR with a reply already sent. */
static int zvParseRangeOptions(client *c, int first, int allowed, zvRangeSpec *r) {
    int has_limit = 0;
    for (int j = first; j < c->argc; j++) {
        char *opt = objectGetVal(c->argv[j]);
        int moreargs = c->argc - 1 - j;
        if ((allowed & ZV_OPT_BY) && r->type == ZV_RANGE_RANK && !strcasecmp(opt, "byscore")) {
            r->type = ZV_RANGE_SCORE;
        } else if ((allowed & ZV_OPT_BY) && r->type == ZV_RANGE_RANK && !strcasecmp(opt, "bylex")) {
            r->type = ZV_RANGE_LEX;
        } else if ((allowed & ZV_OPT_REV) && !strcasecmp(opt, "rev")) {
            r->reverse = 1;
        } else if ((allowed & ZV_OPT_WITHSCORES) && !strcasecmp(opt, "withscores")) {
            r->withscores = 1;
        } else if ((allowed & ZV_OPT_LIMIT) && !has_limit && moreargs >= 2 && !strcasecmp(opt, "limit")) {
            if (getLongFromObjectOrReply(c, c->argv[j + 1], &r->offset, NULL) != C_OK ||
                getLongFromObjectOrReply(c, c->argv[j + 2], &r->count, NULL) != C_OK)
                return C_ERR;
            if (r->offset < 0) {
                addReplyError(c, "LIMIT offset must be non-negative");
                return C_ERR;
            }
            has_limit = 1;
            j += 2;
        } else {
            addReplyErrorObject(c, shared.syntaxerr);
            return C_ERR;
        }
    }
    if (has_limit && r->type == ZV_RANGE_RANK) {
        addReplyError(c, "LIMIT is only supported with BYSCORE or BYLEX");
        return C_ERR;
    }
    return C_OK;
}

static void zvReplyScoreRangeError(client *c) {
    addReplyError(c, "invalid vector score range");
}

static void zvReplyLexRangeError(client *c) {
    addReplyError(c, "min or max not valid string range item");
}

static void zvReplyBylexUniformError(client *c) {
    addReplyError(c, "BYLEX requires all members to share the same vector score");
}

/* Syntax-only check for a BYLEX bound. */
static int zvLexBoundSyntaxOk(const char *str, size_t len) {
    if (len == 1 && (str[0] == '-' || str[0] == '+')) return 1;
    if (len >= 1 && (str[0] == '[' || str[0] == '(')) return 1;
    return 0;
}

/* Syntax-only check for a BYSCORE bound (any dimension count). */
static int zvScoreBoundSyntaxOk(const char *str, size_t len) {
    if (len == 1 && (str[0] == '-' || str[0] == '+')) return 1;
    if (len > 1 && str[0] == '(') {
        str++;
        len--;
    }
    zvScore *s = zvScoreParse(str, len);
    if (s == NULL) return 0;
    zvScoreFree(s);
    return 1;
}

/* Read the range arguments at argv[idx], argv[idx+1] into r. Bounds are
 * validated syntactically so a missing key still reports malformed
 * input, like ZSET. Returns C_OK, or C_ERR with a reply already sent. */
static int zvParseRangeArgs(client *c, int idx, zvRangeSpec *r) {
    if (r->type == ZV_RANGE_RANK) {
        if (getLongFromObjectOrReply(c, c->argv[idx], &r->start, NULL) != C_OK ||
            getLongFromObjectOrReply(c, c->argv[idx + 1], &r->stop, NULL) != C_OK)
            return C_ERR;
        return C_OK;
    }
    /* Reversed commands take max first. */
    r->min = objectGetVal(c->argv[r->reverse ? idx + 1 : idx]);
    r->max = objectGetVal(c->argv[r->reverse ? idx : idx + 1]);
    if (r->type == ZV_RANGE_SCORE) {
        if (!zvScoreBoundSyntaxOk(r->min, sdslen(r->min)) || !zvScoreBoundSyntaxOk(r->max, sdslen(r->max))) {
            zvReplyScoreRangeError(c);
            return C_ERR;
        }
    } else if (!zvLexBoundSyntaxOk(r->min, sdslen(r->min)) || !zvLexBoundSyntaxOk(r->max, sdslen(r->max))) {
        zvReplyLexRangeError(c);
        return C_ERR;
    }
    return C_OK;
}

/* Normalize [start,stop] rank bounds against len. With reverse, ranks
 * count from the head of the reversed set, i.e. forward window
 * [len-1-stop, len-start). */
static void zvRankWindow(unsigned long len, long start, long stop, int reverse, unsigned long *lo, unsigned long *hi) {
    if (start < 0) start = (long)len + start;
    if (stop < 0) stop = (long)len + stop;
    if (start < 0) start = 0;
    if (stop < 0 || start > stop || start >= (long)len) {
        *lo = *hi = 0;
        return;
    }
    if (stop >= (long)len) stop = (long)len - 1;
    if (!reverse) {
        *lo = (unsigned long)start;
        *hi = (unsigned long)stop + 1;
    } else {
        *lo = (unsigned long)((long)len - 1 - stop);
        *hi = (unsigned long)((long)len - start);
    }
}

/* Resolve r against zs to the forward rank window [lo,hi). LIMIT and
 * direction are applied later by zvIterateRange(). Returns C_OK, or
 * C_ERR with a reply already sent. */
static int zvResolveRange(client *c, zvset *zs, const zvRangeSpec *r, unsigned long *lo, unsigned long *hi) {
    switch (r->type) {
    case ZV_RANGE_RANK: zvRankWindow(zvsetLength(zs), r->start, r->stop, r->reverse, lo, hi); return C_OK;
    case ZV_RANGE_SCORE:
        if (zvsetScoreRangeToRanks(zs, r->min, sdslen(r->min), r->max, sdslen(r->max), lo, hi) != C_OK) {
            zvReplyScoreRangeError(c);
            return C_ERR;
        }
        return C_OK;
    case ZV_RANGE_LEX:
        if (!zvsetUniformVector(zs)) {
            zvReplyBylexUniformError(c);
            return C_ERR;
        }
        if (zvsetLexRangeToRanks(zs, r->min, sdslen(r->min), r->max, sdslen(r->max), lo, hi) != C_OK) {
            zvReplyLexRangeError(c);
            return C_ERR;
        }
        return C_OK;
    }
    serverPanic("Unknown zvset range type");
}

/* Number of items zvIterateRange() emits for the same arguments. */
static unsigned long zvRangeResultCount(unsigned long lo, unsigned long hi, long offset, long count) {
    if (lo >= hi || (unsigned long)offset >= hi - lo) return 0;
    unsigned long n = hi - lo - (unsigned long)offset;
    if (count >= 0 && n > (unsigned long)count) n = (unsigned long)count;
    return n;
}

typedef struct zvRangeEmitCtx {
    client *c;
    int withscores;
} zvRangeEmitCtx;

static void zvRangeEmitClient(void *ctx, const_sds item) {
    zvRangeEmitCtx *ectx = ctx;
    addReplyZvElement(ectx->c, item, ectx->withscores);
}

/* Collect packed-item copies for RANGESTORE (dst may be src). */
typedef struct zvStoreCollect {
    sds *items;
    size_t len;
} zvStoreCollect;

static void zvStoreCollectEmit(void *ctx, const_sds item) {
    zvStoreCollect *col = ctx;
    col->items[col->len++] = sdsdup(item);
}

/* Store the collected items at dstkey (replacing any value, like
 * ZRANGESTORE), or delete dstkey when there are none. */
static void zvStoreResult(client *c, robj *dstkey, uint8_t dims, zvStoreCollect *col) {
    if (col->len > 0) {
        robj *newobj = createZvsetObject(dims);
        zvset *newzs = objectGetVal(newobj);
        hashtableExpand(newzs->ht, col->len);
        for (size_t i = 0; i < col->len; i++) {
            sds inserted = fbtreeInsert(newzs->tree, col->items[i]);
            serverAssert(hashtableAdd(newzs->ht, inserted));
        }
        setKey(c, c->db, dstkey, &newobj, 0);
        notifyKeyspaceEvent(NOTIFY_ZSET, "zvrangestore", dstkey, c->db->id);
        server.dirty++;
    } else if (dbDelete(c->db, dstkey)) {
        signalModifiedKey(c, c->db, dstkey);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", dstkey, c->db->id);
        server.dirty++;
    }
    addReplyLongLong(c, (long long)col->len);
}

/* Shared implementation of every ZV*RANGE* read command and
 * ZVRANGESTORE (dstkey != NULL). argv[keyidx] is the source key and
 * the two range arguments follow it. */
static void zvrangeGenericCommand(client *c, robj *dstkey, int keyidx, zvRangeType type, int reverse, int allowed) {
    zvRangeSpec r = {.type = type, .reverse = reverse, .count = -1};
    robj *zobj;

    if (c->argc < keyidx + 3) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }
    if (zvParseRangeOptions(c, keyidx + 3, allowed, &r) != C_OK) return;
    if (zvParseRangeArgs(c, keyidx + 1, &r) != C_OK) return;

    if (dstkey) {
        zobj = lookupKeyWrite(c->db, c->argv[keyidx]);
        if (checkType(c, zobj, OBJ_ZVSET)) return;
        if (zobj == NULL) {
            zvStoreCollect none = {NULL, 0};
            zvStoreResult(c, dstkey, 0, &none);
            return;
        }
    } else if ((zobj = lookupKeyReadOrReply(c, c->argv[keyidx], shared.emptyarray)) == NULL ||
               checkType(c, zobj, OBJ_ZVSET)) {
        return;
    }

    zvset *zs = objectGetVal(zobj);
    unsigned long lo, hi;
    if (zvResolveRange(c, zs, &r, &lo, &hi) != C_OK) return;
    unsigned long n = zvRangeResultCount(lo, hi, r.offset, r.count);

    if (dstkey) {
        /* Collect first: dst may be src. */
        zvStoreCollect col = {n ? zmalloc(sizeof(sds) * n) : NULL, 0};
        zvIterateRange(zs, lo, hi, r.reverse, r.offset, r.count, zvStoreCollectEmit, &col);
        serverAssert(col.len == n);
        zvStoreResult(c, dstkey, zs->dimensions, &col);
        zfree(col.items);
        return;
    }
    addReplyArrayLen(c, (r.withscores && c->resp <= 2) ? n * 2 : n);
    zvRangeEmitCtx ctx = {c, r.withscores};
    zvIterateRange(zs, lo, hi, r.reverse, r.offset, r.count, zvRangeEmitClient, &ctx);
}

/* ZVRANGE key start stop [BYSCORE|BYLEX] [REV] [LIMIT offset count] [WITHSCORES] */
void zvrangeCommand(client *c) {
    zvrangeGenericCommand(c, NULL, 1, ZV_RANGE_RANK, 0, ZV_OPT_BY | ZV_OPT_REV | ZV_OPT_WITHSCORES | ZV_OPT_LIMIT);
}

/* ZVRANGESTORE dst src start stop [BYSCORE|BYLEX] [REV] [LIMIT offset count] */
void zvrangestoreCommand(client *c) {
    zvrangeGenericCommand(c, c->argv[1], 2, ZV_RANGE_RANK, 0, ZV_OPT_BY | ZV_OPT_REV | ZV_OPT_LIMIT);
}

void zvrevrangeCommand(client *c) {
    zvrangeGenericCommand(c, NULL, 1, ZV_RANGE_RANK, 1, ZV_OPT_WITHSCORES);
}

void zvrangebyscoreCommand(client *c) {
    zvrangeGenericCommand(c, NULL, 1, ZV_RANGE_SCORE, 0, ZV_OPT_WITHSCORES | ZV_OPT_LIMIT);
}

void zvrevrangebyscoreCommand(client *c) {
    zvrangeGenericCommand(c, NULL, 1, ZV_RANGE_SCORE, 1, ZV_OPT_WITHSCORES | ZV_OPT_LIMIT);
}

void zvrangebylexCommand(client *c) {
    zvrangeGenericCommand(c, NULL, 1, ZV_RANGE_LEX, 0, ZV_OPT_LIMIT);
}

void zvrevrangebylexCommand(client *c) {
    zvrangeGenericCommand(c, NULL, 1, ZV_RANGE_LEX, 1, ZV_OPT_LIMIT);
}

/* ZVCOUNT / ZVLEXCOUNT: size of the resolved rank window. */
static void zvcountGenericCommand(client *c, zvRangeType type) {
    zvRangeSpec r = {.type = type, .count = -1};
    robj *zobj;

    if (zvParseRangeArgs(c, 2, &r) != C_OK) return;
    if ((zobj = lookupKeyReadOrReply(c, c->argv[1], shared.czero)) == NULL || checkType(c, zobj, OBJ_ZVSET)) return;
    unsigned long lo, hi;
    if (zvResolveRange(c, objectGetVal(zobj), &r, &lo, &hi) != C_OK) return;
    addReplyLongLong(c, (long long)(hi - lo));
}

void zvcountCommand(client *c) {
    zvcountGenericCommand(c, ZV_RANGE_SCORE);
}

void zvlexcountCommand(client *c) {
    zvcountGenericCommand(c, ZV_RANGE_LEX);
}

/* ZVRANKBYSCORE / ZVREVRANKBYSCORE key score: the rank score would take
 * if inserted (TairZset EXZRANKBYSCORE semantics). A missing key ranks
 * every score at 0. */
static void zvrankbyscoreGenericCommand(client *c, int reverse) {
    sds raw = objectGetVal(c->argv[2]);
    zvScore *score = zvScoreParse(raw, sdslen(raw));
    if (score == NULL) {
        addReplyError(c, "invalid vector score value, must be like '1#2#3.5'");
        return;
    }
    robj *zobj = lookupKeyRead(c->db, c->argv[1]);
    if (checkType(c, zobj, OBJ_ZVSET)) {
        zvScoreFree(score);
        return;
    }
    if (zobj == NULL) {
        addReplyLongLong(c, 0);
    } else if (score->len != ((zvset *)objectGetVal(zobj))->dimensions) {
        addReplyError(c, "vector dimension mismatch for existing key");
    } else {
        addReplyLongLong(c, (long long)zvsetRankByScore(objectGetVal(zobj), score, reverse));
    }
    zvScoreFree(score);
}

void zvrankbyscoreCommand(client *c) {
    zvrankbyscoreGenericCommand(c, 0);
}

void zvrevrankbyscoreCommand(client *c) {
    zvrankbyscoreGenericCommand(c, 1);
}

/* Callback for fbtree range deletion: drop the hashtable reference
 * while the packed item is still alive (fbtree frees it after). */
static void zvRangeDeleteCallback(sds item, void *ctx) {
    zvset *zs = ctx;
    serverAssert(hashtableDelete(zs->ht, item));
}

/* ZVREMRANGEBYRANK / ZVREMRANGEBYSCORE / ZVREMRANGEBYLEX. */
static void zvremrangeGenericCommand(client *c, zvRangeType type) {
    static char *events[] = {"zvremrangebyrank", "zvremrangebyscore", "zvremrangebylex"};
    zvRangeSpec r = {.type = type, .count = -1};
    robj *key = c->argv[1];
    robj *zobj;

    if (zvParseRangeArgs(c, 2, &r) != C_OK) return;
    if ((zobj = lookupKeyWriteOrReply(c, key, shared.czero)) == NULL || checkType(c, zobj, OBJ_ZVSET)) return;
    zvset *zs = objectGetVal(zobj);
    unsigned long lo, hi;
    if (zvResolveRange(c, zs, &r, &lo, &hi) != C_OK) return;
    if (lo >= hi) {
        addReply(c, shared.czero);
        return;
    }

    hashtablePauseAutoShrink(zs->ht);
    unsigned long deleted = fbtreeDeleteRangeByRank(zs->tree, lo, hi - 1, zvRangeDeleteCallback, zs);
    int keyremoved = zvsetLength(zs) == 0;
    if (keyremoved) {
        dbDelete(c->db, key);
    } else {
        hashtableResumeAutoShrink(zs->ht);
    }
    signalModifiedKey(c, c->db, key);
    notifyKeyspaceEvent(NOTIFY_ZSET, events[type], key, c->db->id);
    if (keyremoved) notifyKeyspaceEvent(NOTIFY_GENERIC, "del", key, c->db->id);
    server.dirty += deleted;
    addReplyLongLong(c, (long long)deleted);
}

void zvremrangebyrankCommand(client *c) {
    zvremrangeGenericCommand(c, ZV_RANGE_RANK);
}

void zvremrangebyscoreCommand(client *c) {
    zvremrangeGenericCommand(c, ZV_RANGE_SCORE);
}

void zvremrangebylexCommand(client *c) {
    zvremrangeGenericCommand(c, ZV_RANGE_LEX);
}

#define ZVPOP_MIN 0
#define ZVPOP_MAX 1

void zvmpopGenericCommand(client *c, int numkeys_idx, int is_block);
void blockingGenericZvpopCommand(client *c, robj **keys, int numkeys, int where, int timeout_idx, long count, int use_nested_array, int reply_nil_when_empty);

/* Temporary rank->rank map for distinct random sampling (sparse
 * Fisher-Yates). Entries are heap zvRankMapEntry structs freed by the
 * entry destructor on release. */
typedef struct zvRankMapEntry {
    long k;
    long v;
} zvRankMapEntry;

static uint64_t zvRankMapHash(const void *key) {
    uint64_t x = (uint64_t)((const zvRankMapEntry *)key)->k + 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static int zvRankMapCmp(const void *a, const void *b) {
    return ((const zvRankMapEntry *)a)->k == ((const zvRankMapEntry *)b)->k;
}

static void zvRankMapDestructor(void *entry) {
    zfree(entry);
}

static hashtableType zvRankMapHashtableType = {
    .hashFunction = zvRankMapHash,
    .keyCompare = zvRankMapCmp,
    .entryDestructor = zvRankMapDestructor,
};

static unsigned long zvRankMapGet(hashtable *map, unsigned long k, unsigned long def) {
    zvRankMapEntry probe;
    probe.k = (long)k;
    probe.v = 0;
    void *found = NULL;
    if (hashtableFind(map, &probe, &found)) return (unsigned long)((zvRankMapEntry *)found)->v;
    return def;
}

static void zvRankMapSet(hashtable *map, unsigned long k, unsigned long v) {
    zvRankMapEntry probe;
    probe.k = (long)k;
    probe.v = 0;
    void **ref = hashtableFindRef(map, &probe);
    if (ref != NULL) {
        ((zvRankMapEntry *)*ref)->v = (long)v;
        return;
    }
    zvRankMapEntry *e = zmalloc(sizeof(*e));
    e->k = (long)k;
    e->v = (long)v;
    serverAssert(hashtableAdd(map, e));
}

static void zvRankMapFree(hashtable *map) {
    hashtableRelease(map);
}

/* Uniform random integer in [0, n): full 64-bit MT19937 output (the same
 * generator hashtable.c uses) with rejection sampling, so ranks stay
 * uniform beyond RAND_MAX and small ranges have no modulo bias. */
static unsigned long zvRandomBelow(unsigned long n) {
    serverAssert(n > 0);
    unsigned long long limit = ~0ULL - (~0ULL % n);
    unsigned long long x;
    do {
        x = genrand64_int64();
    } while (x >= limit);
    return (unsigned long)(x % n);
}

/* Initial reply header mirroring addZpopInitialReply. */
static void zvPopInitialReply(client *c, int emitkey, int use_nested_array, long rangelen, robj *key) {
    if (!use_nested_array && !emitkey) {
        addReplyArrayLen(c, rangelen * 2);
    } else if (use_nested_array && !emitkey) {
        addReplyArrayLen(c, rangelen);
    } else if (!use_nested_array && emitkey) {
        addReplyArrayLen(c, rangelen * 2 + 1);
        addReplyBulk(c, key);
    } else {
        addReplyArrayLen(c, 2);
        addReplyBulk(c, key);
        addReplyArrayLen(c, rangelen);
    }
}

/* Generic pop used by ZVPOPMIN/ZVPOPMAX/ZVMPOP (and Phase 6 blocking).
 * fbtreePopMin/Max transfer item ownership to us; the hashtable
 * reference is dropped while the item is alive, then it is freed. */
void genericZvpopCommand(client *c,
                         robj **keyv,
                         int keyc,
                         int where,
                         int emitkey,
                         long count,
                         int use_nested_array,
                         int reply_nil_when_empty,
                         int *deleted) {
    int idx;
    robj *key = NULL;
    robj *zobj = NULL;

    if (deleted) *deleted = 0;

    /* First existing key wins; wrong type aborts. */
    idx = 0;
    while (idx < keyc) {
        key = keyv[idx++];
        zobj = lookupKeyWrite(c->db, key);
        if (!zobj) continue;
        if (checkType(c, zobj, OBJ_ZVSET)) return;
        break;
    }

    if (!zobj) {
        if (reply_nil_when_empty) {
            addReplyNullArray(c);
        } else {
            addReply(c, shared.emptyarray);
        }
        return;
    }

    if (count == 0) {
        addReply(c, shared.emptyarray);
        return;
    }
    if (count == -1) count = 1;

    zvset *zs = objectGetVal(zobj);
    long llen = (long)zvsetLength(zs);
    long rangelen = (count > llen) ? llen : count;
    long result_count = 0;

    hashtablePauseAutoShrink(zs->ht);
    do {
        sds item = (where == ZVPOP_MAX) ? fbtreePopMax(zs->tree) : fbtreePopMin(zs->tree);
        serverAssertWithInfo(c, zobj, item != NULL);
        /* Drop the hashtable reference while the item is alive. */
        serverAssert(hashtableDelete(zs->ht, item));
        server.dirty++;

        if (result_count == 0) {
            char *events[2] = {"zvpopmin", "zvpopmax"};
            notifyKeyspaceEvent(NOTIFY_ZSET, events[where], key, c->db->id);
            zvPopInitialReply(c, emitkey, use_nested_array, rangelen, key);
        }

        size_t member_len;
        const char *member = zvItemMember(item, &member_len);
        if (use_nested_array) addReplyArrayLen(c, 2);
        addReplyBulkCBuffer(c, member, member_len);
        addReplyZvScore(c, item);
        sdsfree(item);
        ++result_count;
    } while (--rangelen);

    if (zvsetLength(zs) == 0) {
        if (deleted) *deleted = 1;
        dbDelete(c->db, key);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "del", key, c->db->id);
    } else {
        hashtableResumeAutoShrink(zs->ht);
    }
    signalModifiedKey(c, c->db, key);

    if (c->cmd->proc == zvmpopCommand) {
        /* Replicate as ZVPOPMIN/ZVPOPMAX with COUNT. */
        robj *popcmd = createStringObject(where == ZVPOP_MAX ? "ZVPOPMAX" : "ZVPOPMIN", 8);
        robj *count_obj = createStringObjectFromLongLong((count > llen) ? llen : count);
        rewriteClientCommandVector(c, 3, popcmd, key, count_obj);
        decrRefCount(popcmd);
        decrRefCount(count_obj);
    }
}

void zvpopMinMaxCommand(client *c, int where) {
    if (c->argc > 3) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }

    long count = -1; /* -1 for plain single pop. */
    if (c->argc == 3 && getPositiveLongFromObjectOrReply(c, c->argv[2], &count, NULL) != C_OK) return;

    /* Flat array in RESP2 or for single pops; nested in RESP3 with COUNT. */
    int use_nested_array = (c->resp > 2 && count != -1);

    genericZvpopCommand(c, &c->argv[1], 1, where, 0, count, use_nested_array, 0, NULL);
}

void zvpopminCommand(client *c) {
    zvpopMinMaxCommand(c, ZVPOP_MIN);
}

void zvpopmaxCommand(client *c) {
    zvpopMinMaxCommand(c, ZVPOP_MAX);
}

void zvmpopCommand(client *c) {
    zvmpopGenericCommand(c, 1, 0);
}

void bzvmpopCommand(client *c) {
    zvmpopGenericCommand(c, 2, 1);
}

void zvmpopGenericCommand(client *c, int numkeys_idx, int is_block) {
    long j;
    long numkeys = 0;
    int where = ZVPOP_MIN;
    long count = -1;

    if (getRangeLongFromObjectOrReply(c, c->argv[numkeys_idx], 1, LONG_MAX, &numkeys,
                                      "numkeys should be greater than 0") != C_OK)
        return;

    if (numkeys > c->argc - (numkeys_idx + 2)) {
        addReplyError(c, "numkeys should be less than the number of arguments");
        return;
    }

    long where_idx = numkeys_idx + numkeys + 1;
    if (where_idx >= c->argc) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }
    if (!strcasecmp(objectGetVal(c->argv[where_idx]), "MIN")) {
        where = ZVPOP_MIN;
    } else if (!strcasecmp(objectGetVal(c->argv[where_idx]), "MAX")) {
        where = ZVPOP_MAX;
    } else {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }

    for (j = where_idx + 1; j < c->argc; j++) {
        char *opt = objectGetVal(c->argv[j]);
        int moreargs = (c->argc - 1) - j;
        if (count == -1 && !strcasecmp(opt, "COUNT") && moreargs) {
            j++;
            if (getRangeLongFromObjectOrReply(c, c->argv[j], 1, LONG_MAX, &count, "count should be greater than 0") !=
                C_OK)
                return;
        } else {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
    }

    if (count == -1) count = 1;

    if (is_block) {
        blockingGenericZvpopCommand(c, c->argv + numkeys_idx + 1, (int)numkeys, where, 1, count, 1, 1);
    } else {
        genericZvpopCommand(c, c->argv + numkeys_idx + 1, (int)numkeys, where, 1, count, 1, 1, NULL);
    }
}

/* BZVPOPMIN, BZVPOPMAX, BZVMPOP actual implementation. */
void blockingGenericZvpopCommand(client *c, robj **keys, int numkeys, int where, int timeout_idx, long count, int use_nested_array, int reply_nil_when_empty) {
    robj *o;
    robj *key;
    mstime_t timeout;
    int j;

    if (getTimeoutFromObjectOrReply(c, c->argv[timeout_idx], &timeout, UNIT_SECONDS) != C_OK) return;

    for (j = 0; j < numkeys; j++) {
        key = keys[j];
        o = lookupKeyWrite(c->db, key);
        if (o == NULL) continue;

        if (checkType(c, o, OBJ_ZVSET)) return;

        long llen = (long)zvsetObjectLength(o);
        if (llen == 0) continue;

        genericZvpopCommand(c, &key, 1, where, 1, count, use_nested_array, reply_nil_when_empty, NULL);

        if (count == -1) {
            robj *popcmd = createStringObject(where == ZVPOP_MAX ? "ZVPOPMAX" : "ZVPOPMIN", 8);
            rewriteClientCommandVector(c, 2, popcmd, key);
            decrRefCount(popcmd);
        } else {
            robj *popcmd = createStringObject(where == ZVPOP_MAX ? "ZVPOPMAX" : "ZVPOPMIN", 8);
            robj *count_obj = createStringObjectFromLongLong((count > llen) ? llen : count);
            rewriteClientCommandVector(c, 3, popcmd, key, count_obj);
            decrRefCount(popcmd);
            decrRefCount(count_obj);
        }
        return;
    }

    /* Not allowed to block: treat as timeout (even with timeout 0). */
    if (c->flag.deny_blocking) {
        addReplyNullArray(c);
        return;
    }

    blockForKeys(c, BLOCKED_ZVSET, keys, numkeys, timeout, 0);
}

void bzvpopminCommand(client *c) {
    blockingGenericZvpopCommand(c, c->argv + 1, c->argc - 2, ZVPOP_MIN, c->argc - 1, -1, 0, 0);
}

void bzvpopmaxCommand(client *c) {
    blockingGenericZvpopCommand(c, c->argv + 1, c->argc - 2, ZVPOP_MAX, c->argc - 1, -1, 0, 0);
}

void zvrandmemberCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;
    long count = 0;
    int withscores = 0;
    int single = 0;

    if (c->argc == 2) {
        single = 1;
    } else if (c->argc <= 4) {
        if (getLongFromObjectOrReply(c, c->argv[2], &count, NULL) != C_OK) return;
        if (c->argc == 4) {
            if (strcasecmp(objectGetVal(c->argv[3]), "withscores")) {
                addReplyErrorObject(c, shared.syntaxerr);
                return;
            }
            withscores = 1;
            /* Cap withscores counts like ZRANDMEMBER: want*2 must not
             * wrap the RESP array length. */
            if (count < -LONG_MAX / 2 || count > LONG_MAX / 2) {
                addReplyError(c, "value is out of range");
                return;
            }
        }
    } else {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }

    if ((zobj = lookupKeyReadOrReply(c, key, single ? shared.null[c->resp] : shared.emptyarray)) == NULL ||
        checkType(c, zobj, OBJ_ZVSET))
        return;

    zvset *zs = objectGetVal(zobj);
    unsigned long len = zvsetLength(zs);
    if (len == 0) {
        if (single)
            addReplyNull(c);
        else
            addReplyArrayLen(c, 0);
        return;
    }

    /* A magnitude of LONG_MIN cannot be served: its absolute value does
     * not fit in the RESP array length (long). Reject it instead of
     * wrapping to a negative length (or looping forever). */
    if (!single && count == LONG_MIN) {
        addReplyError(c, "value is out of range");
        return;
    }

    if (single) {
        const_sds item = fbtreeGetAtRank(zs->tree, zvRandomBelow(len));
        serverAssert(item != NULL);
        size_t member_len;
        const char *member = zvItemMember(item, &member_len);
        addReplyBulkCBuffer(c, member, member_len);
        return;
    }

    int allow_dup = count < 0;
    unsigned long want = allow_dup ? (unsigned long)(-(count + 1)) + 1 : (unsigned long)count;
    if (!allow_dup && want >= len) {
        /* Return everything in shuffled order. */
        want = len;
    }

    if (want == 0) {
        addReplyArrayLen(c, 0);
        return;
    }

    if (allow_dup || want >= len / 4) {
        /* Dense distinct samples use a pointer array: the tree is walked
         * once rather than searched independently for every chosen rank.
         * Sparse samples below retain the smaller temporary rank map. */
        const_sds *items = NULL;
        if (!allow_dup) {
            items = zmalloc(sizeof(*items) * len);
            fbtreeIterator it;
            fbtreeInitIterator(&it, zs->tree);
            for (unsigned long i = 0; i < len; i++) {
                items[i] = fbtreeNext(&it);
                serverAssert(items[i] != NULL);
            }
        }
        addReplyArrayLen(c, (withscores && c->resp <= 2) ? want * 2 : want);
        for (unsigned long i = 0; i < want; i++) {
            const_sds item;
            if (allow_dup) {
                item = fbtreeGetAtRank(zs->tree, zvRandomBelow(len));
            } else {
                /* Partial Fisher-Yates preserves uniform sampling without
                 * replacement, including the full-set permutation. */
                unsigned long j = i + zvRandomBelow(len - i);
                item = items[j];
                items[j] = items[i];
                items[i] = item;
            }
            serverAssert(item != NULL);
            addReplyZvElement(c, item, withscores);
            if (c->flag.close_asap) break;
        }
        zfree(items);
        return;
    }

    /* Distinct partial sample: sparse Fisher-Yates with a temporary
     * rank->rank map backed by a small hashtable. Position i is fixed by
     * swapping in a random later position j; the value moved into i (vj)
     * is emitted. Position i is never read again, so only j needs to be
     * stored for future swaps. */
    hashtable *map = hashtableCreate(&zvRankMapHashtableType);
    addReplyArrayLen(c, (withscores && c->resp <= 2) ? want * 2 : want);
    for (unsigned long i = 0; i < want; i++) {
        unsigned long j = i + zvRandomBelow(len - i);
        unsigned long vi = zvRankMapGet(map, i, i);
        unsigned long vj = zvRankMapGet(map, j, j);
        if (j != i) zvRankMapSet(map, j, vi);
        const_sds item = fbtreeGetAtRank(zs->tree, vj);
        serverAssert(item != NULL);
        addReplyZvElement(c, item, withscores);
        if (c->flag.close_asap) break;
    }
    zvRankMapFree(map);
}

void zvscanCommand(client *c) {
    robj *o;
    unsigned long long cursor;

    if (parseScanCursorOrReply(c, objectGetVal(c->argv[2]), &cursor) == C_ERR) return;
    if ((o = lookupKeyReadOrReply(c, c->argv[1], shared.emptyscan)) == NULL || checkType(c, o, OBJ_ZVSET)) return;
    scanGenericCommand(c, o, cursor);
}

/* Multi-dimensional filter scan. The leading dimension uses the tree's
 * vector order to restrict the scan without a secondary index. */
typedef struct zvQueryFilter {
    long dimension;
    int min_ex, max_ex;
    int min_unbounded, max_unbounded;
    uint64_t min, max; /* Host-endian sortable scores. */
} zvQueryFilter;

/* Parse one scalar bound: "-", "+", "(1.5" or "1.5". NaN rejected. */
static int zvQueryParseBound(const char *str, size_t len, int is_min, uint64_t *val, int *exclusive, int *unbounded) {
    *exclusive = 0;
    *unbounded = 0;
    if (len == 1 && ((is_min && str[0] == '-') || (!is_min && str[0] == '+'))) {
        *unbounded = 1;
        return C_OK;
    }
    if (len > 1 && str[0] == '(') {
        *exclusive = 1;
        str++;
        len--;
    }
    double v;
    if (!string2d(str, len, &v)) return C_ERR;
    *val = ntohu64(zvScoreToSortable(v));
    return C_OK;
}

static int zvQueryMatches(const zvQueryFilter *filters, int nfilters, const_sds item) {
    for (int i = 0; i < nfilters; i++) {
        const zvQueryFilter *f = &filters[i];
        /* Compare the canonical ordered bits directly, without decoding
         * a double and undoing the sign transform for every candidate. */
        uint64_t value;
        memcpy(&value, item + 1 + (size_t)f->dimension * 8, sizeof(value));
        value = ntohu64(value);
        if (!f->min_unbounded && (value < f->min || (value == f->min && f->min_ex))) return 0;
        if (!f->max_unbounded && (value > f->max || (value == f->max && f->max_ex))) return 0;
    }
    return 1;
}

void zvqueryCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;
    zvQueryFilter filters[ZVSET_MAX_DIMENSIONS];
    int nfilters = 0;
    long offset = 0, count = -1;
    int withscores = 0;

    if (c->argc < 2) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }
    int idx = 2;
    while (idx < c->argc) {
        char *opt = objectGetVal(c->argv[idx]);
        if (!strcasecmp(opt, "filter")) {
            if (nfilters >= ZVSET_MAX_DIMENSIONS || idx + 4 > c->argc) {
                addReplyErrorObject(c, shared.syntaxerr);
                return;
            }
            long dim;
            if (getLongFromObjectOrReply(c, c->argv[idx + 1], &dim, NULL) != C_OK) return;
            if (dim < 0) {
                addReplyError(c, "filter dimension must be non-negative");
                return;
            }
            zvQueryFilter *f = &filters[nfilters];
            f->dimension = dim;
            sds mins = objectGetVal(c->argv[idx + 2]);
            sds maxs = objectGetVal(c->argv[idx + 3]);
            if (zvQueryParseBound(mins, sdslen(mins), 1, &f->min, &f->min_ex, &f->min_unbounded) != C_OK ||
                zvQueryParseBound(maxs, sdslen(maxs), 0, &f->max, &f->max_ex, &f->max_unbounded) != C_OK) {
                addReplyError(c, "invalid filter bound");
                return;
            }
            nfilters++;
            idx += 4;
        } else if (!strcasecmp(opt, "limit")) {
            if (idx + 2 >= c->argc) {
                addReplyErrorObject(c, shared.syntaxerr);
                return;
            }
            if (getLongFromObjectOrReply(c, c->argv[idx + 1], &offset, NULL) != C_OK) return;
            if (getLongFromObjectOrReply(c, c->argv[idx + 2], &count, NULL) != C_OK) return;
            if (offset < 0) {
                addReplyError(c, "LIMIT offset must be non-negative");
                return;
            }
            idx += 3;
        } else if (!strcasecmp(opt, "withscores")) {
            withscores = 1;
            idx++;
        } else {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
    }

    if ((zobj = lookupKeyReadOrReply(c, key, shared.emptyarray)) == NULL || checkType(c, zobj, OBJ_ZVSET))
        return;
    zvset *zs = objectGetVal(zobj);

    /* Validate dimensions and trivially-reversed filters up front. */
    for (int i = 0; i < nfilters; i++) {
        if (filters[i].dimension >= zs->dimensions) {
            addReplyError(c, "filter dimension out of range");
            return;
        }
        if (!filters[i].min_unbounded && !filters[i].max_unbounded &&
            (filters[i].min > filters[i].max ||
             (filters[i].min == filters[i].max && (filters[i].min_ex || filters[i].max_ex)))) {
            addReplyArrayLen(c, 0);
            return;
        }
    }

    if (count == 0) {
        addReplyArrayLen(c, 0);
        return;
    }

    /* Intersect all bounds on dimension zero. A short packed prefix
     * includes every suffix with that score; its byte successor excludes
     * every suffix, including members with embedded NULs. */
    sds lower = NULL, upper = NULL;
    for (int i = 0; i < nfilters; i++) {
        const zvQueryFilter *f = &filters[i];
        if (f->dimension != 0) continue;
        for (int side = 0; side < 2; side++) {
            if (side ? f->max_unbounded : f->min_unbounded) continue;
            char prefix[9];
            prefix[0] = (char)zs->dimensions;
            uint64_t score = htonu64(side ? f->max : f->min);
            memcpy(prefix + 1, &score, sizeof(score));
            sds bound = sdsnewlen(prefix, sizeof(prefix));
            if (side ? !f->max_ex : f->min_ex) {
                sds successor = zvBoundSuccessor(bound);
                sdsfree(bound);
                bound = successor;
            }
            sds *target = side ? &upper : &lower;
            if (*target == NULL || (side ? sdscmp(bound, *target) < 0 : sdscmp(bound, *target) > 0)) {
                sdsfree(*target);
                *target = bound;
            } else {
                sdsfree(bound);
            }
        }
    }
    unsigned long lo, hi;
    zvScoreRanks(zs, lower, upper, &lo, &hi);
    sdsfree(lower);
    sdsfree(upper);

    /* Dimension-zero predicates are guaranteed by the indexed bounds.
     * When no residual predicates remain, LIMIT offset is a rank seek
     * rather than a walk through every skipped match. */
    int residual = 0;
    for (int i = 0; i < nfilters; i++) {
        if (filters[i].dimension != 0) filters[residual++] = filters[i];
    }
    nfilters = residual;
    if (nfilters == 0) {
        unsigned long skip = (unsigned long)offset;
        if (skip > hi - lo) skip = hi - lo;
        lo += skip;
        offset = 0;
    }
    if (lo == hi) {
        addReplyArrayLen(c, 0);
        return;
    }

    /* Stream in vector order with no transient copy of the result set. */
    void *arraylen_ptr = addReplyDeferredLen(c);
    long emitted = 0;
    long skipped = 0;
    fbtreeIterator it;
    fbtreeInitIterator(&it, zs->tree);
    if (lo) fbtreeSeekToRank(&it, lo);
    const_sds item;
    unsigned long remaining = hi - lo;
    while (remaining-- && (item = fbtreeNext(&it)) != NULL) {
        if (!zvQueryMatches(filters, nfilters, item)) continue;
        if (skipped < offset) {
            skipped++;
            continue;
        }
        addReplyZvElement(c, item, withscores);
        emitted++;
        if (count >= 0 && emitted >= count) break;
    }
    setDeferredArrayLen(c, arraylen_ptr, (withscores && c->resp <= 2) ? emitted * 2 : emitted);
}

#define ZV_OP_UNION 0
#define ZV_OP_INTER 1
#define ZV_OP_DIFF 2

typedef struct zvSetOpSrc {
    robj *subject; /* NULL for missing keys (treated as empty). */
    double weight;
} zvSetOpSrc;

static unsigned long zvSetOpSrcLen(zvSetOpSrc *src) {
    if (src->subject == NULL) return 0;
    return zvsetLength(objectGetVal(src->subject));
}

/* Insert the accumulated vector for member into the temp result.
 * The temp object holds unique members, so this is insert-only. */
static void zvSetOpResultInsert(zvset *res, const zvScore *vec, const char *member, size_t member_len) {
    sds item = zvItemCreate(vec, member, member_len);
    sds inserted = fbtreeInsert(res->tree, item);
    serverAssert(hashtableAdd(res->ht, inserted));
}

static void zvsetopGenericCommand(client *c, robj *dstkey, int numkeysIndex, int op, int cardinality_only) {
    long setnum = 0;
    int aggregate = ZV_AGGR_SUM;
    int withscores = 0;
    long limit = 0;
    zvSetOpSrc *src = NULL;

    if (getLongFromObjectOrReply(c, c->argv[numkeysIndex], &setnum, NULL) != C_OK) return;
    if (setnum < 1) {
        addReplyErrorFormat(c, "at least 1 input key is needed for '%s' command", c->cmd->fullname);
        return;
    }
    if (setnum > (c->argc - (numkeysIndex + 1))) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }

    src = zcalloc(sizeof(*src) * (size_t)setnum);
    int j = numkeysIndex + 1;
    for (long i = 0; i < setnum; i++, j++) {
        robj *obj = lookupKeyRead(c->db, c->argv[j]);
        if (obj != NULL) {
            if (objectGetType(obj) != OBJ_ZVSET) {
                zfree(src);
                addReplyErrorObject(c, shared.wrongtypeerr);
                return;
            }
            src[i].subject = obj;
        } else {
            src[i].subject = NULL;
        }
        src[i].weight = 1.0;
    }

    /* All non-empty inputs must share dimensions (missing keys excluded). */
    uint8_t dims = 0;
    for (long i = 0; i < setnum; i++) {
        if (src[i].subject == NULL) continue;
        uint8_t d = ((zvset *)objectGetVal(src[i].subject))->dimensions;
        if (dims == 0) {
            dims = d;
        } else if (dims != d) {
            zfree(src);
            addReplyError(c, "input keys must have the same vector dimension");
            return;
        }
    }

    /* Parse optional arguments. */
    if (j < c->argc) {
        int remaining = c->argc - j;
        while (remaining) {
            if (op != ZV_OP_DIFF && !cardinality_only && remaining >= (setnum + 1) &&
                !strcasecmp(objectGetVal(c->argv[j]), "weights")) {
                j++;
                remaining--;
                for (long i = 0; i < setnum; i++, j++, remaining--) {
                    if (getDoubleFromObjectOrReply(c, c->argv[j], &src[i].weight, "weight value is not a float") !=
                        C_OK) {
                        zfree(src);
                        return;
                    }
                }
            } else if (op != ZV_OP_DIFF && !cardinality_only && remaining >= 2 &&
                       !strcasecmp(objectGetVal(c->argv[j]), "aggregate")) {
                j++;
                remaining--;
                if (!strcasecmp(objectGetVal(c->argv[j]), "sum")) {
                    aggregate = ZV_AGGR_SUM;
                } else if (!strcasecmp(objectGetVal(c->argv[j]), "min")) {
                    aggregate = ZV_AGGR_MIN;
                } else if (!strcasecmp(objectGetVal(c->argv[j]), "max")) {
                    aggregate = ZV_AGGR_MAX;
                } else {
                    zfree(src);
                    addReplyErrorObject(c, shared.syntaxerr);
                    return;
                }
                j++;
                remaining--;
            } else if (remaining >= 1 && !dstkey && !cardinality_only &&
                       !strcasecmp(objectGetVal(c->argv[j]), "withscores")) {
                j++;
                remaining--;
                withscores = 1;
            } else if (cardinality_only && remaining >= 2 && !strcasecmp(objectGetVal(c->argv[j]), "limit")) {
                j++;
                remaining--;
                if (getPositiveLongFromObjectOrReply(c, c->argv[j], &limit, "LIMIT can't be negative") != C_OK) {
                    zfree(src);
                    return;
                }
                j++;
                remaining--;
            } else {
                zfree(src);
                addReplyErrorObject(c, shared.syntaxerr);
                return;
            }
        }
    }

    /* INTERCARD: count only, smallest input drives, early exit at LIMIT. */
    if (cardinality_only) {
        if (setnum == 1) {
            unsigned long cardinality = zvSetOpSrcLen(&src[0]);
            if (limit && cardinality > (unsigned long)limit) cardinality = (unsigned long)limit;
            zfree(src);
            addReplyLongLong(c, (long long)cardinality);
            return;
        }
        long driver = -1;
        for (long i = 0; i < setnum; i++) {
            if (src[i].subject == NULL) {
                driver = -2; /* Empty input: result is 0. */
                break;
            }
            if (driver == -1 || zvSetOpSrcLen(&src[i]) < zvSetOpSrcLen(&src[driver])) driver = i;
        }
        unsigned long cardinality = 0;
        if (driver >= 0 && zvSetOpSrcLen(&src[driver]) > 0) {
            zvset *dzs = objectGetVal(src[driver].subject);
            hashtableIterator iter;
            hashtableInitIterator(&iter, dzs->ht, 0);
            void *next;
            while (hashtableNext(&iter, &next)) {
                int present = 1;
                for (long i = 0; i < setnum; i++) {
                    if (i == driver) continue;
                    if (src[i].subject == NULL) {
                        present = 0;
                        break;
                    }
                    zvset *izs = objectGetVal(src[i].subject);
                    if (!hashtableFind(izs->ht, next, NULL)) {
                        present = 0;
                        break;
                    }
                }
                if (!present) continue;
                cardinality++;
                if (limit && cardinality >= (unsigned long)limit) break;
            }
            hashtableCleanupIterator(&iter);
        }
        zfree(src);
        addReplyLongLong(c, (long long)cardinality);
        return;
    }

    /* Build the temp result. Nothing is mutated until the destination
     * replacement below, so input validation failures keep dst intact. */
    robj *tmpobj = createZvsetObject(dims ? dims : 1);
    zvset *tmp = objectGetVal(tmpobj);
    zvScore *scratch_a = dims ? zvScoreCreate(dims) : NULL;
    zvScore *scratch_b = dims ? zvScoreCreate(dims) : NULL;
    zvScore *scratch_c = dims ? zvScoreCreate(dims) : NULL;

    if (op == ZV_OP_UNION && dims) {
        unsigned long total = 0;
        for (long i = 0; i < setnum; i++) total += zvSetOpSrcLen(&src[i]);
        if (total) hashtableExpand(tmp->ht, total);
        for (long i = 0; i < setnum; i++) {
            if (src[i].subject == NULL) continue;
            zvset *izs = objectGetVal(src[i].subject);
            hashtableIterator iter;
            hashtableInitIterator(&iter, izs->ht, 0);
            void *next;
            while (hashtableNext(&iter, &next)) {
                size_t mlen;
                const char *member = zvItemMember(next, &mlen);
                zvItemToScore(next, scratch_a);
                zvScoreApplyWeight(scratch_b, scratch_a, src[i].weight);
                /* Packed items already extract to their member, so the
                 * input item is also an allocation-free lookup key. */
                void **slot = hashtableFindRef(tmp->ht, next);
                if (slot == NULL) {
                    sds item = zvItemCreate(scratch_b, member, mlen);
                    serverAssert(hashtableAdd(tmp->ht, item));
                } else {
                    zvItemToScore(*slot, scratch_a);
                    zvScoreAggregate(scratch_a, scratch_b, aggregate);
                    /* Only the member participates in hashing. Before
                     * tree insertion, scores can be updated in place. */
                    sds item = *slot;
                    for (int d = 0; d < dims; d++) {
                        uint64_t score = zvScoreToSortable(scratch_a->values[d]);
                        memcpy(item + 1 + (size_t)d * 8, &score, sizeof(score));
                    }
                }
            }
            hashtableCleanupIterator(&iter);
        }
        /* Transfer every accumulated item to the owning tree once. No
         * temporary result escapes before the tree owns all its items. */
        hashtableIterator iter;
        hashtableInitIterator(&iter, tmp->ht, 0);
        void *next;
        while (hashtableNext(&iter, &next)) {
            serverAssert(fbtreeInsert(tmp->tree, next) == next);
        }
        hashtableCleanupIterator(&iter);
    } else if (op == ZV_OP_INTER && dims) {
        /* Smallest non-empty input drives. */
        long driver = -1;
        for (long i = 0; i < setnum; i++) {
            if (src[i].subject == NULL) {
                driver = -2;
                break;
            }
            if (driver == -1 || zvSetOpSrcLen(&src[i]) < zvSetOpSrcLen(&src[driver])) driver = i;
        }
        if (driver >= 0 && zvSetOpSrcLen(&src[driver]) > 0) {
            zvset *dzs = objectGetVal(src[driver].subject);
            hashtableExpand(tmp->ht, zvSetOpSrcLen(&src[driver]));
            hashtableIterator iter;
            hashtableInitIterator(&iter, dzs->ht, 0);
            void *next;
            while (hashtableNext(&iter, &next)) {
                size_t mlen;
                const char *member = zvItemMember(next, &mlen);
                zvItemToScore(next, scratch_a);
                zvScoreApplyWeight(scratch_c, scratch_a, src[driver].weight);
                int k;
                for (k = 0; k < setnum; k++) {
                    if (k == driver) continue;
                    zvset *izs = objectGetVal(src[k].subject);
                    void *found;
                    if (!hashtableFind(izs->ht, next, &found)) break;
                    zvItemToScore(found, scratch_a);
                    zvScoreApplyWeight(scratch_b, scratch_a, src[k].weight);
                    zvScoreAggregate(scratch_c, scratch_b, aggregate);
                }
                if (k == setnum) zvSetOpResultInsert(tmp, scratch_c, member, mlen);
            }
            hashtableCleanupIterator(&iter);
        }
    } else if (op == ZV_OP_DIFF && dims) {
        /* Members of the first input absent from every later input. */
        if (src[0].subject != NULL) {
            zvset *first = objectGetVal(src[0].subject);
            hashtableExpand(tmp->ht, zvSetOpSrcLen(&src[0]));
            hashtableIterator iter;
            hashtableInitIterator(&iter, first->ht, 0);
            void *next;
            while (hashtableNext(&iter, &next)) {
                int k;
                for (k = 1; k < setnum; k++) {
                    if (src[k].subject == NULL) continue;
                    zvset *izs = objectGetVal(src[k].subject);
                    if (hashtableFind(izs->ht, next, NULL)) break;
                }
                if (k == setnum) {
                    sds copy = sdsdup(next);
                    sds inserted = fbtreeInsert(tmp->tree, copy);
                    serverAssert(hashtableAdd(tmp->ht, inserted));
                }
            }
            hashtableCleanupIterator(&iter);
        }
    }

    if (scratch_a) zvScoreFree(scratch_a);
    if (scratch_b) zvScoreFree(scratch_b);
    if (scratch_c) zvScoreFree(scratch_c);

    static const char *opnames[3] = {"zvunionstore", "zvinterstore", "zvdiffstore"};
    if (dstkey) {
        unsigned long resultlen = zvsetLength(tmp);
        if (resultlen) {
            setKey(c, c->db, dstkey, &tmpobj, 0);
            notifyKeyspaceEvent(NOTIFY_ZSET, (char *)opnames[op], dstkey, c->db->id);
            addReplyLongLong(c, (long long)resultlen);
            server.dirty++;
        } else {
            if (dbDelete(c->db, dstkey)) {
                signalModifiedKey(c, c->db, dstkey);
                notifyKeyspaceEvent(NOTIFY_GENERIC, "del", dstkey, c->db->id);
                server.dirty++;
            }
            addReply(c, shared.czero);
            decrRefCount(tmpobj);
        }
    } else {
        unsigned long length = zvsetLength(tmp);
        if (withscores && c->resp == 2)
            addReplyArrayLen(c, length * 2);
        else
            addReplyArrayLen(c, length);
        fbtreeIterator it;
        fbtreeInitIterator(&it, tmp->tree);
        const_sds item;
        while ((item = fbtreeNext(&it)) != NULL) {
            addReplyZvElement(c, item, withscores);
        }
        decrRefCount(tmpobj);
    }
    zfree(src);
}

void zvunionCommand(client *c) {
    zvsetopGenericCommand(c, NULL, 1, ZV_OP_UNION, 0);
}

void zvunionstoreCommand(client *c) {
    zvsetopGenericCommand(c, c->argv[1], 2, ZV_OP_UNION, 0);
}

void zvinterCommand(client *c) {
    zvsetopGenericCommand(c, NULL, 1, ZV_OP_INTER, 0);
}

void zvinterstoreCommand(client *c) {
    zvsetopGenericCommand(c, c->argv[1], 2, ZV_OP_INTER, 0);
}

void zvdiffCommand(client *c) {
    zvsetopGenericCommand(c, NULL, 1, ZV_OP_DIFF, 0);
}

void zvdiffstoreCommand(client *c) {
    zvsetopGenericCommand(c, c->argv[1], 2, ZV_OP_DIFF, 0);
}

void zvintercardCommand(client *c) {
    zvsetopGenericCommand(c, NULL, 1, ZV_OP_INTER, 1);
}

void zvcardCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;

    if ((zobj = lookupKeyReadOrReply(c, key, shared.czero)) == NULL || checkType(c, zobj, OBJ_ZVSET))
        return;
    zvset *zs = objectGetVal(zobj);
    /* The hashtable and the tree must always agree. */
    serverAssert(hashtableSize(zs->ht) == fbtreeLength(zs->tree));
    addReplyLongLong(c, (long long)hashtableSize(zs->ht));
}

/* AOF rewrite helper: emit ZVADD commands. Called from aof.c. */
int rewriteZvsetObject(rio *r, robj *key, robj *o) {
    long long count = 0;
    zvset *zs = objectGetVal(o);
    unsigned long items = zvsetLength(zs);

    hashtableIterator iter;
    hashtableInitIterator(&iter, zs->ht, 0);
    void *next;
    while (hashtableNext(&iter, &next)) {
        const_sds item = next;
        if (count == 0) {
            unsigned long cmd_items = items > (unsigned long)AOF_REWRITE_ITEMS_PER_CMD
                                          ? (unsigned long)AOF_REWRITE_ITEMS_PER_CMD
                                          : items;
            if (!rioWriteBulkCount(r, '*', 2 + cmd_items * 2) || !rioWriteBulkString(r, "ZVADD", 5) ||
                !rioWriteBulkObject(r, key)) {
                hashtableCleanupIterator(&iter);
                return 0;
            }
        }
        char buf[ZVSET_SCORE_BUFLEN];
        size_t buflen = zvItemFormatScoreBuf(item, buf);
        size_t member_len;
        const char *member = zvItemMember(item, &member_len);
        int ok = rioWriteBulkString(r, buf, buflen) && rioWriteBulkString(r, member, member_len);
        if (!ok) {
            hashtableCleanupIterator(&iter);
            return 0;
        }
        if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
        items--;
    }
    hashtableCleanupIterator(&iter);
    return 1;
}
