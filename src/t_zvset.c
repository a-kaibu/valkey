#include "server.h"
#include "zvset.h"
#include "sds.h"
#include "hashtable.h"
#include "fbtree.h"
#include "rio.h"
#include "zmalloc.h"

#include <strings.h>

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

    /* Parse optional flags. Accept NX/XX/GT/LT/CH/INCR in any order. */
    while (scoreidx < c->argc) {
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
            for (int k = 0; k <= j; k++) zvScoreFree(scores[k]);
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
        notifyKeyspaceEvent(NOTIFY_GENERIC, incr ? "zvincrby" : "zvadd", key, c->db->id);
        server.dirty += (added + updated);
    }
    if (reply_err) {
        if (incr_result) zvScoreFree(incr_result);
        addReplyError(c, "increment would produce NaN");
    } else if (incr) {
        if (processed) {
            sds formatted = zvScoreFormat(incr_result);
            addReplyBulkSds(c, formatted);
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
        notifyKeyspaceEvent(NOTIFY_GENERIC, "zvrem", key, c->db->id);
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
    sds formatted = zvItemFormatScore(entry);
    addReplyBulkSds(c, formatted);
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
        sds formatted = zvItemFormatScore(entry);
        addReplyBulkSds(c, formatted);
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
    sds formatted = zvItemFormatScore(entry);
    addReplyBulkSds(c, formatted);
}

void zvrankCommand(client *c) {
    zvrankGenericCommand(c, 0);
}

void zvrevrankCommand(client *c) {
    zvrankGenericCommand(c, 1);
}

/* Unified ZVRANGE core. Rank mode uses start/stop indexes; BYSCORE mode
 * uses already-parsed bounds. RESP array length is computed up front. */
typedef struct zvRangeEmitCtx {
    client *c;
    int withscores;
} zvRangeEmitCtx;

static void zvRangeEmitClient(void *ctx, const_sds item) {
    zvRangeEmitCtx *ectx = ctx;
    size_t member_len;
    const char *member = zvItemMember(item, &member_len);
    addReplyBulkCBuffer(ectx->c, member, member_len);
    if (ectx->withscores) {
        sds formatted = zvItemFormatScore(item);
        addReplyBulkSds(ectx->c, formatted);
    }
}

static unsigned long zvRangeResultCount(unsigned long lo, unsigned long hi, int reverse, long offset, long count) {
    if (lo >= hi) return 0;
    if (offset < 0) offset = 0;
    if (!reverse) {
        unsigned long start = lo + (unsigned long)offset;
        if (start >= hi) return 0;
        unsigned long n = hi - start;
        if (count >= 0 && n > (unsigned long)count) n = (unsigned long)count;
        return n;
    }
    long start = (long)hi - 1 - offset;
    if (start < (long)lo) return 0;
    unsigned long n = (unsigned long)(start - (long)lo) + 1;
    if (count >= 0 && n > (unsigned long)count) n = (unsigned long)count;
    return n;
}

/* Emit [lo,hi) with direction/limit. All rank normalization and bound
 * resolution must be done by the caller. */
static void zvrangeReply(client *c, zvset *zs, unsigned long lo, unsigned long hi, int reverse, long offset, long count, int withscores) {
    unsigned long n = zvRangeResultCount(lo, hi, reverse, offset, count);
    addReplyArrayLen(c, withscores ? n * 2 : n);
    if (n == 0) return;
    zvRangeEmitCtx ctx = {c, withscores};
    zvIterateRange(zs, lo, hi, reverse, offset, count, zvRangeEmitClient, &ctx);
}

/* Resolve BYSCORE lower/upper bound SDS pair from parsed bounds.
 * Returns 1 with *empty=1 when the range is trivially empty.
 * Caller frees *lower and *upper (NULL when unbounded). */
static int zvBuildScoreInterval(zvScoreBound *minb, zvScoreBound *maxb, sds *lower, sds *upper, int *empty) {
    *lower = NULL;
    *upper = NULL;
    *empty = 0;
    if (!minb->unbounded && !maxb->unbounded) {
        int cmp = zvScoreCompare(minb->score, maxb->score);
        if (cmp > 0 || (cmp == 0 && (minb->exclusive || maxb->exclusive))) {
            *empty = 1;
            return 1;
        }
    }
    if (!minb->unbounded) {
        sds prefix = zvBuildScorePrefix(minb->score);
        if (minb->exclusive) {
            *lower = zvBoundSuccessor(prefix);
            sdsfree(prefix);
            if (*lower == NULL) {
                /* Exclusive min beyond byte space: nothing can follow. */
                *empty = 1;
                return 1;
            }
        } else {
            *lower = prefix;
        }
    }
    if (!maxb->unbounded) {
        sds prefix = zvBuildScorePrefix(maxb->score);
        if (maxb->exclusive) {
            *upper = prefix;
        } else {
            *upper = zvBoundSuccessor(prefix);
            sdsfree(prefix);
            if (*upper == NULL) {
                /* Inclusive max covers everything above: unbounded. */
                *upper = NULL;
            }
        }
    }
    return 1;
}

/* Forward declarations for the shared range cores. */
static void zvrangeRankCore(client *c, robj *zobj, long start, long stop, int reverse, int withscores);
static int zvrangeScoreCore(client *c, robj *zobj, const char *minraw, size_t minlen, const char *maxraw, size_t maxlen, int reverse, int withscores, int has_limit, long offset, long count);
static int zvrangeLexCore(client *c, robj *zobj, const char *minraw, size_t minlen, const char *maxraw, size_t maxlen, int reverse, int withscores, int has_limit, long offset, long count);

/* Parsed trailing range options shared by ZVRANGE/ZVRANGESTORE. */
typedef struct zvRangeOpts {
    int mode; /* 0 = rank, 1 = byscore, 2 = bylex */
    int reverse;
    int withscores;
    int has_limit;
    long offset;
    long count;
} zvRangeOpts;

/* Parse c->argv[first..argc) as range options. allow_withscores gates
 * WITHSCORES (rejected for STORE). Returns C_OK, or C_ERR with a reply
 * already sent. LIMIT requires BYSCORE/BYLEX. */
static int zvRangeParseOpts(client *c, int first, int allow_withscores, zvRangeOpts *o) {
    o->mode = 0;
    o->reverse = 0;
    o->withscores = 0;
    o->has_limit = 0;
    o->offset = 0;
    o->count = -1;
    int idx = first;
    while (idx < c->argc) {
        char *opt = objectGetVal(c->argv[idx]);
        if (!strcasecmp(opt, "byscore")) {
            if (o->mode != 0) {
                addReplyErrorObject(c, shared.syntaxerr);
                return C_ERR;
            }
            o->mode = 1;
            idx++;
        } else if (!strcasecmp(opt, "bylex")) {
            if (o->mode != 0) {
                addReplyErrorObject(c, shared.syntaxerr);
                return C_ERR;
            }
            o->mode = 2;
            idx++;
        } else if (!strcasecmp(opt, "rev")) {
            o->reverse = 1;
            idx++;
        } else if (!strcasecmp(opt, "withscores")) {
            if (!allow_withscores) {
                addReplyErrorObject(c, shared.syntaxerr);
                return C_ERR;
            }
            o->withscores = 1;
            idx++;
        } else if (!strcasecmp(opt, "limit")) {
            if (o->has_limit || idx + 2 >= c->argc) {
                addReplyErrorObject(c, shared.syntaxerr);
                return C_ERR;
            }
            if (getLongFromObjectOrReply(c, c->argv[idx + 1], &o->offset, NULL) != C_OK) return C_ERR;
            if (getLongFromObjectOrReply(c, c->argv[idx + 2], &o->count, NULL) != C_OK) return C_ERR;
            if (o->offset < 0) {
                addReplyError(c, "LIMIT offset must be non-negative");
                return C_ERR;
            }
            o->has_limit = 1;
            idx += 3;
        } else {
            addReplyErrorObject(c, shared.syntaxerr);
            return C_ERR;
        }
    }
    if (o->has_limit && o->mode == 0) {
        addReplyError(c, "LIMIT is only supported with BYSCORE or BYLEX");
        return C_ERR;
    }
    return C_OK;
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

void zvrangeCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;
    long start = 0, stop = -1;
    zvRangeOpts o;

    if (c->argc < 4) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }
    if (zvRangeParseOpts(c, 4, 1, &o) != C_OK) return;

    sds bound_min_raw = NULL, bound_max_raw = NULL;
    size_t bound_min_len = 0, bound_max_len = 0;
    if (o.mode == 0) {
        if (getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != C_OK) return;
        if (getLongFromObjectOrReply(c, c->argv[3], &stop, NULL) != C_OK) return;
    } else {
        /* REV takes max first, like ZRANGE. */
        int minarg = o.reverse ? 3 : 2;
        int maxarg = o.reverse ? 2 : 3;
        bound_min_raw = objectGetVal(c->argv[minarg]);
        bound_min_len = sdslen(bound_min_raw);
        bound_max_raw = objectGetVal(c->argv[maxarg]);
        bound_max_len = sdslen(bound_max_raw);
        /* Validate bound syntax even for missing keys. */
        if (o.mode == 1) {
            if (!zvScoreBoundSyntaxOk(bound_min_raw, bound_min_len) ||
                !zvScoreBoundSyntaxOk(bound_max_raw, bound_max_len)) {
                addReplyError(c, "invalid vector score range");
                return;
            }
        } else {
            if (!zvLexBoundSyntaxOk(bound_min_raw, bound_min_len) ||
                !zvLexBoundSyntaxOk(bound_max_raw, bound_max_len)) {
                addReplyError(c, "min or max not valid string range item");
                return;
            }
        }
    }
    if ((zobj = lookupKeyReadOrReply(c, key, shared.emptyarray)) == NULL || checkType(c, zobj, OBJ_ZVSET))
        return;
    if (o.mode == 0) {
        zvrangeRankCore(c, zobj, start, stop, o.reverse, o.withscores);
        return;
    }
    if (o.mode == 1) {
        if (zvrangeScoreCore(c, zobj, bound_min_raw, bound_min_len, bound_max_raw, bound_max_len, o.reverse,
                             o.withscores, o.has_limit, o.offset, o.count) != C_OK) {
            addReplyError(c, "invalid vector score range");
            return;
        }
        return;
    }
    if (zvrangeLexCore(c, zobj, bound_min_raw, bound_min_len, bound_max_raw, bound_max_len, o.reverse,
                       o.withscores, o.has_limit, o.offset, o.count) != C_OK) {
        /* Error reply already sent by the core. */
        return;
    }
}

/* Rank core shared by ZVRANGE/ZVREVRANGE. */
static void zvrangeRankCore(client *c, robj *zobj, long start, long stop, int reverse, int withscores) {
    zvset *zs = objectGetVal(zobj);
    unsigned long len = zvsetLength(zs);
    /* Normalize negative indexes like ZRANGE. */
    if (start < 0) start = (long)len + start;
    if (stop < 0) stop = (long)len + stop;
    if (start < 0) start = 0;
    unsigned long lo, hi;
    if (stop < 0 || start > stop || start >= (long)len) {
        lo = hi = 0;
    } else {
        if (stop >= (long)len) stop = (long)len - 1;
        lo = (unsigned long)start;
        hi = (unsigned long)stop + 1;
    }
    zvrangeReply(c, zs, lo, hi, reverse, 0, -1, withscores);
}

/* BYSCORE core shared by ZVRANGE/ZVRANGEBYSCORE/ZVREVRANGEBYSCORE.
 * Returns C_OK, or C_ERR when bounds are invalid for the key. */
static int zvrangeScoreCore(client *c, robj *zobj, const char *minraw, size_t minlen, const char *maxraw, size_t maxlen, int reverse, int withscores, int has_limit, long offset, long count) {
    zvset *zs = objectGetVal(zobj);
    zvScoreBound minb = {0}, maxb = {0};
    if (zvParseScoreBound(minraw, minlen, zs->dimensions, &minb) != C_OK ||
        zvParseScoreBound(maxraw, maxlen, zs->dimensions, &maxb) != C_OK) {
        zvFreeScoreBound(&minb);
        zvFreeScoreBound(&maxb);
        return C_ERR;
    }
    sds lower = NULL, upper = NULL;
    int empty = 0;
    zvBuildScoreInterval(&minb, &maxb, &lower, &upper, &empty);
    zvFreeScoreBound(&minb);
    zvFreeScoreBound(&maxb);
    if (empty) {
        if (lower) sdsfree(lower);
        if (upper) sdsfree(upper);
        addReplyArrayLen(c, 0);
        return C_OK;
    }
    unsigned long lo, hi;
    zvScoreRanks(zs, lower, upper, &lo, &hi);
    if (lower) sdsfree(lower);
    if (upper) sdsfree(upper);
    long lim_off = has_limit ? offset : 0;
    long lim_cnt = has_limit ? count : -1;
    zvrangeReply(c, zs, lo, hi, reverse, lim_off, lim_cnt, withscores);
    return C_OK;
}

void zvrevrangeCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;
    long start, stop;
    int withscores = 0;

    if (c->argc < 4 || c->argc > 5) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }
    if (getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != C_OK) return;
    if (getLongFromObjectOrReply(c, c->argv[3], &stop, NULL) != C_OK) return;
    if (c->argc == 5) {
        if (strcasecmp(objectGetVal(c->argv[4]), "withscores")) {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
        withscores = 1;
    }
    if ((zobj = lookupKeyReadOrReply(c, key, shared.emptyarray)) == NULL || checkType(c, zobj, OBJ_ZVSET))
        return;
    zvrangeRankCore(c, zobj, start, stop, 1, withscores);
}

/* Shared BYSCORE wrapper core: ZVRANGEBYSCORE (reverse=0) /
 * ZVREVRANGEBYSCORE (reverse=1, args max min). */
static void zvrangebyscoreGenericCommand(client *c, int reverse) {
    robj *key = c->argv[1];
    robj *zobj;
    int withscores = 0;
    int has_limit = 0;
    long offset = 0, count = -1;

    if (c->argc < 4) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }
    int idx = 4;
    while (idx < c->argc) {
        char *opt = objectGetVal(c->argv[idx]);
        if (!strcasecmp(opt, "withscores")) {
            withscores = 1;
            idx++;
        } else if (!strcasecmp(opt, "limit")) {
            if (has_limit || idx + 2 >= c->argc) {
                addReplyErrorObject(c, shared.syntaxerr);
                return;
            }
            if (getLongFromObjectOrReply(c, c->argv[idx + 1], &offset, NULL) != C_OK) return;
            if (getLongFromObjectOrReply(c, c->argv[idx + 2], &count, NULL) != C_OK) return;
            if (offset < 0) {
                addReplyError(c, "LIMIT offset must be non-negative");
                return;
            }
            has_limit = 1;
            idx += 3;
        } else {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
    }
    int minarg = reverse ? 3 : 2;
    int maxarg = reverse ? 2 : 3;
    sds minraw = objectGetVal(c->argv[minarg]);
    sds maxraw = objectGetVal(c->argv[maxarg]);
    if (!zvScoreBoundSyntaxOk(minraw, sdslen(minraw)) || !zvScoreBoundSyntaxOk(maxraw, sdslen(maxraw))) {
        addReplyError(c, "invalid vector score range");
        return;
    }
    if ((zobj = lookupKeyReadOrReply(c, key, shared.emptyarray)) == NULL || checkType(c, zobj, OBJ_ZVSET))
        return;
    if (zvrangeScoreCore(c, zobj, minraw, sdslen(minraw), maxraw, sdslen(maxraw), reverse, withscores, has_limit,
                         offset, count) != C_OK) {
        addReplyError(c, "invalid vector score range");
        return;
    }
}

void zvrangebyscoreCommand(client *c) {
    zvrangebyscoreGenericCommand(c, 0);
}

void zvrevrangebyscoreCommand(client *c) {
    zvrangebyscoreGenericCommand(c, 1);
}

/* Build [lower,upper) seek keys for uniform-vector BYLEX. Returns C_OK;
 * *empty is set when the range is trivially empty. The key must be
 * non-empty and uniform (checked by the caller). */
static int zvBuildLexInterval(zvset *zs, zvLexBound *minb, zvLexBound *maxb, sds *lower, sds *upper, int *empty) {
    *lower = NULL;
    *upper = NULL;
    *empty = 0;
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
            *lower = zvBoundSuccessor(key);
            sdsfree(key);
            if (*lower == NULL) {
                *empty = 1;
                return C_OK;
            }
        } else {
            *lower = key;
        }
    }
    if (!maxb->unbounded) {
        sds key = zvLexSeekKey(zs, maxb->member, sdslen(maxb->member));
        if (maxb->exclusive) {
            *upper = key;
        } else {
            *upper = zvBoundSuccessor(key);
            sdsfree(key);
            if (*upper == NULL) *upper = NULL; /* Covers everything above. */
        }
    }
    return C_OK;
}

/* Error message shared by every BYLEX entry point. */
static void zvReplyBylexUniformError(client *c) {
    addReplyError(c, "BYLEX requires all members to share the same vector score");
}

/* BYLEX core shared by ZVRANGE/ZVRANGEBYLEX/ZVREVRANGEBYLEX/ZVLEXCOUNT.
 * Withscores is always 0 for BYLEX (members only). Returns C_OK, or
 * C_ERR with a reply already sent. */
static int zvrangeLexCore(client *c, robj *zobj, const char *minraw, size_t minlen, const char *maxraw, size_t maxlen, int reverse, int withscores, int has_limit, long offset, long count) {
    zvset *zs = objectGetVal(zobj);
    if (!zvsetUniformVector(zs)) {
        zvReplyBylexUniformError(c);
        return C_ERR;
    }
    zvLexBound minb = {0}, maxb = {0};
    if (zvParseLexBound(minraw, minlen, &minb) != C_OK || zvParseLexBound(maxraw, maxlen, &maxb) != C_OK) {
        zvFreeLexBound(&minb);
        zvFreeLexBound(&maxb);
        addReplyError(c, "min or max not valid string range item");
        return C_ERR;
    }
    sds lower = NULL, upper = NULL;
    int empty = 0;
    zvBuildLexInterval(zs, &minb, &maxb, &lower, &upper, &empty);
    zvFreeLexBound(&minb);
    zvFreeLexBound(&maxb);
    if (empty) {
        if (lower) sdsfree(lower);
        if (upper) sdsfree(upper);
        addReplyArrayLen(c, 0);
        return C_OK;
    }
    unsigned long lo, hi;
    zvScoreRanks(zs, lower, upper, &lo, &hi);
    if (lower) sdsfree(lower);
    if (upper) sdsfree(upper);
    long lim_off = has_limit ? offset : 0;
    long lim_cnt = has_limit ? count : -1;
    zvrangeReply(c, zs, lo, hi, reverse, lim_off, lim_cnt, withscores);
    return C_OK;
}

/* Shared BYLEX wrapper core: ZVRANGEBYLEX (reverse=0) /
 * ZVREVRANGEBYLEX (reverse=1, args max min). */
static void zvrangebylexGenericCommand(client *c, int reverse) {
    robj *key = c->argv[1];
    robj *zobj;
    int has_limit = 0;
    long offset = 0, count = -1;

    if (c->argc < 4) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }
    int idx = 4;
    while (idx < c->argc) {
        char *opt = objectGetVal(c->argv[idx]);
        if (!strcasecmp(opt, "limit")) {
            if (has_limit || idx + 2 >= c->argc) {
                addReplyErrorObject(c, shared.syntaxerr);
                return;
            }
            if (getLongFromObjectOrReply(c, c->argv[idx + 1], &offset, NULL) != C_OK) return;
            if (getLongFromObjectOrReply(c, c->argv[idx + 2], &count, NULL) != C_OK) return;
            if (offset < 0) {
                addReplyError(c, "LIMIT offset must be non-negative");
                return;
            }
            has_limit = 1;
            idx += 3;
        } else {
            addReplyErrorObject(c, shared.syntaxerr);
            return;
        }
    }
    int minarg = reverse ? 3 : 2;
    int maxarg = reverse ? 2 : 3;
    sds minraw = objectGetVal(c->argv[minarg]);
    sds maxraw = objectGetVal(c->argv[maxarg]);
    if (!zvLexBoundSyntaxOk(minraw, sdslen(minraw)) || !zvLexBoundSyntaxOk(maxraw, sdslen(maxraw))) {
        addReplyError(c, "min or max not valid string range item");
        return;
    }
    if ((zobj = lookupKeyReadOrReply(c, key, shared.emptyarray)) == NULL || checkType(c, zobj, OBJ_ZVSET))
        return;
    zvrangeLexCore(c, zobj, minraw, sdslen(minraw), maxraw, sdslen(maxraw), reverse, 0, has_limit, offset, count);
}

void zvrangebylexCommand(client *c) {
    zvrangebylexGenericCommand(c, 0);
}

void zvrevrangebylexCommand(client *c) {
    zvrangebylexGenericCommand(c, 1);
}

void zvlexcountCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;

    if (c->argc != 4) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }
    sds minraw = objectGetVal(c->argv[2]);
    sds maxraw = objectGetVal(c->argv[3]);
    if (!zvLexBoundSyntaxOk(minraw, sdslen(minraw)) || !zvLexBoundSyntaxOk(maxraw, sdslen(maxraw))) {
        addReplyError(c, "min or max not valid string range item");
        return;
    }
    if ((zobj = lookupKeyReadOrReply(c, key, shared.czero)) == NULL || checkType(c, zobj, OBJ_ZVSET)) return;
    zvset *zs = objectGetVal(zobj);
    if (!zvsetUniformVector(zs)) {
        zvReplyBylexUniformError(c);
        return;
    }
    zvLexBound minb = {0}, maxb = {0};
    if (zvParseLexBound(minraw, sdslen(minraw), &minb) != C_OK ||
        zvParseLexBound(maxraw, sdslen(maxraw), &maxb) != C_OK) {
        zvFreeLexBound(&minb);
        zvFreeLexBound(&maxb);
        addReplyError(c, "min or max not valid string range item");
        return;
    }
    sds lower = NULL, upper = NULL;
    int empty = 0;
    zvBuildLexInterval(zs, &minb, &maxb, &lower, &upper, &empty);
    zvFreeLexBound(&minb);
    zvFreeLexBound(&maxb);
    long long count;
    if (empty) {
        count = 0;
    } else if (lower != NULL && upper != NULL) {
        count = (long long)fbtreeCountRangeByValue(zs->tree, lower, upper, 0, 1);
    } else {
        unsigned long lo, hi;
        zvScoreRanks(zs, lower, upper, &lo, &hi);
        count = (long long)(hi - lo);
    }
    if (lower) sdsfree(lower);
    if (upper) sdsfree(upper);
    addReplyLongLong(c, count);
}

/* Collect packed-item copies for RANGESTORE (src may be replaced). */
typedef struct zvStoreCollect {
    sds *items;
    size_t len;
    size_t cap;
} zvStoreCollect;

static void zvStoreCollectEmit(void *ctx, const_sds item) {
    zvStoreCollect *col = ctx;
    if (col->len == col->cap) {
        col->cap = col->cap ? col->cap * 2 : 16;
        col->items = zrealloc(col->items, sizeof(sds) * col->cap);
    }
    col->items[col->len++] = sdsdup(item);
}

void zvrangestoreCommand(client *c) {
    robj *dstkey = c->argv[1];
    robj *srckey = c->argv[2];
    robj *srcobj, *dstobj;
    zvRangeOpts o;

    if (c->argc < 5) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }
    if (zvRangeParseOpts(c, 5, 0, &o) != C_OK) return;

    long start = 0, stop = -1;
    sds bound_min_raw = NULL, bound_max_raw = NULL;
    size_t bound_min_len = 0, bound_max_len = 0;
    if (o.mode == 0) {
        if (getLongFromObjectOrReply(c, c->argv[3], &start, NULL) != C_OK) return;
        if (getLongFromObjectOrReply(c, c->argv[4], &stop, NULL) != C_OK) return;
    } else {
        int minarg = o.reverse ? 4 : 3;
        int maxarg = o.reverse ? 3 : 4;
        bound_min_raw = objectGetVal(c->argv[minarg]);
        bound_min_len = sdslen(bound_min_raw);
        bound_max_raw = objectGetVal(c->argv[maxarg]);
        bound_max_len = sdslen(bound_max_raw);
        if (o.mode == 1) {
            if (!zvScoreBoundSyntaxOk(bound_min_raw, bound_min_len) ||
                !zvScoreBoundSyntaxOk(bound_max_raw, bound_max_len)) {
                addReplyError(c, "invalid vector score range");
                return;
            }
        } else {
            if (!zvLexBoundSyntaxOk(bound_min_raw, bound_min_len) ||
                !zvLexBoundSyntaxOk(bound_max_raw, bound_max_len)) {
                addReplyError(c, "min or max not valid string range item");
                return;
            }
        }
    }

    /* Missing source: delete destination, return 0 (like ZRANGESTORE). */
    if ((srcobj = lookupKeyWrite(c->db, srckey)) == NULL) {
        if (lookupKeyWrite(c->db, dstkey) != NULL) dbDelete(c->db, dstkey);
        addReplyLongLong(c, 0);
        return;
    }
    if (checkType(c, srcobj, OBJ_ZVSET)) return;
    dstobj = lookupKeyWrite(c->db, dstkey);
    if (dstobj != NULL && checkType(c, dstobj, OBJ_ZVSET)) return;

    zvset *src = objectGetVal(srcobj);
    unsigned long lo = 0, hi = 0;
    if (o.mode == 0) {
        unsigned long len = zvsetLength(src);
        long s = start, e = stop;
        if (s < 0) s = (long)len + s;
        if (e < 0) e = (long)len + e;
        if (s < 0) s = 0;
        if (!(e < 0 || s > e || s >= (long)len)) {
            if (e >= (long)len) e = (long)len - 1;
            lo = (unsigned long)s;
            hi = (unsigned long)e + 1;
        }
    } else if (o.mode == 1) {
        zvScoreBound minb = {0}, maxb = {0};
        if (zvParseScoreBound(bound_min_raw, bound_min_len, src->dimensions, &minb) != C_OK ||
            zvParseScoreBound(bound_max_raw, bound_max_len, src->dimensions, &maxb) != C_OK) {
            zvFreeScoreBound(&minb);
            zvFreeScoreBound(&maxb);
            addReplyError(c, "invalid vector score range");
            return;
        }
        sds lower = NULL, upper = NULL;
        int empty = 0;
        zvBuildScoreInterval(&minb, &maxb, &lower, &upper, &empty);
        zvFreeScoreBound(&minb);
        zvFreeScoreBound(&maxb);
        if (!empty) zvScoreRanks(src, lower, upper, &lo, &hi);
        if (lower) sdsfree(lower);
        if (upper) sdsfree(upper);
    } else {
        if (!zvsetUniformVector(src)) {
            zvReplyBylexUniformError(c);
            return;
        }
        zvLexBound minb = {0}, maxb = {0};
        if (zvParseLexBound(bound_min_raw, bound_min_len, &minb) != C_OK ||
            zvParseLexBound(bound_max_raw, bound_max_len, &maxb) != C_OK) {
            zvFreeLexBound(&minb);
            zvFreeLexBound(&maxb);
            addReplyError(c, "min or max not valid string range item");
            return;
        }
        sds lower = NULL, upper = NULL;
        int empty = 0;
        zvBuildLexInterval(src, &minb, &maxb, &lower, &upper, &empty);
        zvFreeLexBound(&minb);
        zvFreeLexBound(&maxb);
        if (!empty) zvScoreRanks(src, lower, upper, &lo, &hi);
        if (lower) sdsfree(lower);
        if (upper) sdsfree(upper);
    }

    /* Collect first: dst may be src. */
    zvStoreCollect col = {NULL, 0, 0};
    long lim_off = o.has_limit ? o.offset : 0;
    long lim_cnt = o.has_limit ? o.count : -1;
    zvIterateRange(src, lo, hi, o.reverse, lim_off, lim_cnt, zvStoreCollectEmit, &col);

    uint8_t dims = src->dimensions;
    /* Replace wholesale, even on dimension change. */
    if (lookupKeyWrite(c->db, dstkey) != NULL) dbDelete(c->db, dstkey);
    if (col.len > 0) {
        robj *newobj = createZvsetObject(dims);
        zvset *newzs = objectGetVal(newobj);
        hashtableExpand(newzs->ht, col.len);
        for (size_t i = 0; i < col.len; i++) {
            sds inserted = fbtreeInsert(newzs->tree, col.items[i]);
            serverAssert(hashtableAdd(newzs->ht, inserted));
        }
        zfree(col.items);
        dbAdd(c->db, dstkey, &newobj);
        signalModifiedKey(c, c->db, dstkey);
        notifyKeyspaceEvent(NOTIFY_GENERIC, "zvrangestore", dstkey, c->db->id);
        server.dirty++;
    } else {
        zfree(col.items);
    }
    addReplyLongLong(c, (long long)col.len);
}

/* Callback for fbtree range deletion: drop the hashtable reference
 * while the packed item is still alive (fbtree frees it after). */
static void zvRangeDeleteCallback(sds item, void *ctx) {
    zvset *zs = ctx;
    serverAssert(hashtableDelete(zs->ht, item));
}

/* Shared tail for REMRANGE*: pause shrink held, key deletion, notify. */
static void zvremrangeFinish(client *c, robj *key, zvset *zs, unsigned long deleted, char *event) {
    int keyremoved = 0;
    if (hashtableSize(zs->ht) == 0) {
        dbDelete(c->db, key);
        keyremoved = 1;
    } else {
        hashtableResumeAutoShrink(zs->ht);
    }
    if (deleted) {
        signalModifiedKey(c, c->db, key);
        notifyKeyspaceEvent(NOTIFY_GENERIC, event, key, c->db->id);
        if (keyremoved) notifyKeyspaceEvent(NOTIFY_GENERIC, "del", key, c->db->id);
        server.dirty += deleted;
    }
    addReplyLongLong(c, (long long)deleted);
}

void zvremrangebyrankCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;
    long start, stop;
    unsigned long deleted = 0;

    if (c->argc != 4) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }
    if (getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != C_OK) return;
    if (getLongFromObjectOrReply(c, c->argv[3], &stop, NULL) != C_OK) return;
    if ((zobj = lookupKeyWriteOrReply(c, key, shared.czero)) == NULL || checkType(c, zobj, OBJ_ZVSET)) return;

    zvset *zs = objectGetVal(zobj);
    long len = (long)zvsetLength(zs);
    if (start < 0) start = len + start;
    if (stop < 0) stop = len + stop;
    if (start < 0) start = 0;
    if (!(stop < 0 || start > stop || start >= len)) {
        if (stop >= len) stop = len - 1;
        hashtablePauseAutoShrink(zs->ht);
        deleted = fbtreeDeleteRangeByRank(zs->tree, (unsigned long)start, (unsigned long)stop,
                                          zvRangeDeleteCallback, zs);
        zvremrangeFinish(c, key, zs, deleted, "zvremrangebyrank");
        return;
    }
    addReply(c, shared.czero);
}

void zvremrangebyscoreCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;
    unsigned long deleted = 0;

    if (c->argc != 4) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }
    sds minraw = objectGetVal(c->argv[2]);
    sds maxraw = objectGetVal(c->argv[3]);
    if (!zvScoreBoundSyntaxOk(minraw, sdslen(minraw)) || !zvScoreBoundSyntaxOk(maxraw, sdslen(maxraw))) {
        addReplyError(c, "invalid vector score range");
        return;
    }
    if ((zobj = lookupKeyWriteOrReply(c, key, shared.czero)) == NULL || checkType(c, zobj, OBJ_ZVSET)) return;
    zvset *zs = objectGetVal(zobj);
    zvScoreBound minb = {0}, maxb = {0};
    if (zvParseScoreBound(minraw, sdslen(minraw), zs->dimensions, &minb) != C_OK ||
        zvParseScoreBound(maxraw, sdslen(maxraw), zs->dimensions, &maxb) != C_OK) {
        zvFreeScoreBound(&minb);
        zvFreeScoreBound(&maxb);
        addReplyError(c, "invalid vector score range");
        return;
    }
    sds lower = NULL, upper = NULL;
    int empty = 0;
    zvBuildScoreInterval(&minb, &maxb, &lower, &upper, &empty);
    zvFreeScoreBound(&minb);
    zvFreeScoreBound(&maxb);
    hashtablePauseAutoShrink(zs->ht);
    if (!empty) {
        if (lower != NULL && upper != NULL) {
            deleted = fbtreeDeleteRangeByValue(zs->tree, lower, upper, 0, 1, zvRangeDeleteCallback, zs);
        } else {
            unsigned long lo, hi;
            zvScoreRanks(zs, lower, upper, &lo, &hi);
            if (lo < hi) {
                deleted = fbtreeDeleteRangeByRank(zs->tree, lo, hi - 1, zvRangeDeleteCallback, zs);
            }
        }
    }
    if (lower) sdsfree(lower);
    if (upper) sdsfree(upper);
    zvremrangeFinish(c, key, zs, deleted, "zvremrangebyscore");
}

void zvremrangebylexCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;
    unsigned long deleted = 0;

    if (c->argc != 4) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }
    sds minraw = objectGetVal(c->argv[2]);
    sds maxraw = objectGetVal(c->argv[3]);
    if (!zvLexBoundSyntaxOk(minraw, sdslen(minraw)) || !zvLexBoundSyntaxOk(maxraw, sdslen(maxraw))) {
        addReplyError(c, "min or max not valid string range item");
        return;
    }
    if ((zobj = lookupKeyWriteOrReply(c, key, shared.czero)) == NULL || checkType(c, zobj, OBJ_ZVSET)) return;
    zvset *zs = objectGetVal(zobj);
    if (!zvsetUniformVector(zs)) {
        zvReplyBylexUniformError(c);
        return;
    }
    zvLexBound minb = {0}, maxb = {0};
    if (zvParseLexBound(minraw, sdslen(minraw), &minb) != C_OK ||
        zvParseLexBound(maxraw, sdslen(maxraw), &maxb) != C_OK) {
        zvFreeLexBound(&minb);
        zvFreeLexBound(&maxb);
        addReplyError(c, "min or max not valid string range item");
        return;
    }
    sds lower = NULL, upper = NULL;
    int empty = 0;
    zvBuildLexInterval(zs, &minb, &maxb, &lower, &upper, &empty);
    zvFreeLexBound(&minb);
    zvFreeLexBound(&maxb);
    hashtablePauseAutoShrink(zs->ht);
    if (!empty) {
        if (lower != NULL && upper != NULL) {
            deleted = fbtreeDeleteRangeByValue(zs->tree, lower, upper, 0, 1, zvRangeDeleteCallback, zs);
        } else {
            unsigned long lo, hi;
            zvScoreRanks(zs, lower, upper, &lo, &hi);
            if (lo < hi) {
                deleted = fbtreeDeleteRangeByRank(zs->tree, lo, hi - 1, zvRangeDeleteCallback, zs);
            }
        }
    }
    if (lower) sdsfree(lower);
    if (upper) sdsfree(upper);
    zvremrangeFinish(c, key, zs, deleted, "zvremrangebylex");
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
        size_t member_len;
        const char *member = zvItemMember(item, &member_len);
        sds score = zvItemFormatScore(item);
        /* Drop the hashtable reference while the item is alive. */
        serverAssert(hashtableDelete(zs->ht, item));
        server.dirty++;

        if (result_count == 0) {
            char *events[2] = {"zvpopmin", "zvpopmax"};
            notifyKeyspaceEvent(NOTIFY_GENERIC, events[where], key, c->db->id);
            zvPopInitialReply(c, emitkey, use_nested_array, rangelen, key);
        }

        if (use_nested_array) addReplyArrayLen(c, 2);
        addReplyBulkCBuffer(c, member, member_len);
        addReplyBulkSds(c, score);
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

    blockForKeys(c, BLOCKED_ZSET, keys, numkeys, timeout, 0);
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

    if (single) {
        const_sds item = fbtreeGetAtRank(zs->tree, (unsigned long)rand() % len);
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

    if (allow_dup || want >= len) {
        /* Duplicates allowed, or full-set shuffle. */
        unsigned long *ranks = NULL;
        if (!allow_dup) {
            ranks = zmalloc(sizeof(*ranks) * len);
            for (unsigned long i = 0; i < len; i++) ranks[i] = i;
            for (unsigned long i = 0; i < len; i++) {
                unsigned long j = i + (unsigned long)rand() % (len - i);
                unsigned long tmp = ranks[i];
                ranks[i] = ranks[j];
                ranks[j] = tmp;
            }
        }
        addReplyArrayLen(c, withscores ? want * 2 : want);
        for (unsigned long i = 0; i < want; i++) {
            unsigned long r = allow_dup ? (unsigned long)rand() % len : ranks[i];
            const_sds item = fbtreeGetAtRank(zs->tree, r);
            serverAssert(item != NULL);
            size_t member_len;
            const char *member = zvItemMember(item, &member_len);
            addReplyBulkCBuffer(c, member, member_len);
            if (withscores) {
                sds formatted = zvItemFormatScore(item);
                addReplyBulkSds(c, formatted);
            }
        }
        zfree(ranks);
        return;
    }

    /* Distinct partial sample: sparse Fisher-Yates with a temporary
     * rank->rank map backed by a small hashtable. Position i is fixed by
     * swapping in a random later position j; the value moved into i (vj)
     * is emitted. */
    hashtable *map = hashtableCreate(&zvRankMapHashtableType);
    addReplyArrayLen(c, withscores ? want * 2 : want);
    for (unsigned long i = 0; i < want; i++) {
        unsigned long j = i + (unsigned long)rand() % (len - i);
        unsigned long vi = zvRankMapGet(map, i, i);
        unsigned long vj = zvRankMapGet(map, j, j);
        zvRankMapSet(map, i, vj);
        zvRankMapSet(map, j, vi);
        const_sds item = fbtreeGetAtRank(zs->tree, vj);
        serverAssert(item != NULL);
        size_t member_len;
        const char *member = zvItemMember(item, &member_len);
        addReplyBulkCBuffer(c, member, member_len);
        if (withscores) {
            sds formatted = zvItemFormatScore(item);
            addReplyBulkSds(c, formatted);
        }
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

/* Experimental multi-dimensional filter scan (full-scan reference).
 * No secondary index: every item is visited in vector order. */
typedef struct zvQueryFilter {
    long dimension;
    int min_ex, max_ex;
    int min_unbounded, max_unbounded;
    double min, max;
} zvQueryFilter;

/* Parse one scalar bound: "-", "+", "(1.5" or "1.5". NaN rejected. */
static int zvQueryParseBound(const char *str, size_t len, int is_min, double *val, int *exclusive, int *unbounded) {
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
    *val = v;
    return C_OK;
}

static int zvQueryMatches(const zvQueryFilter *filters, int nfilters, const_sds item) {
    for (int i = 0; i < nfilters; i++) {
        const zvQueryFilter *f = &filters[i];
        double value = zvItemScoreAt(item, (uint8_t)f->dimension);
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

    /* Full scan in vector order, streamed with a deferred length so no
     * transient copy of the result set is needed. */
    void *arraylen_ptr = addReplyDeferredLen(c);
    long emitted = 0;
    long skipped = 0;
    fbtreeIterator it;
    fbtreeInitIterator(&it, zs->tree);
    const_sds item;
    while ((item = fbtreeNext(&it)) != NULL) {
        if (!zvQueryMatches(filters, nfilters, item)) continue;
        if (skipped < offset) {
            skipped++;
            continue;
        }
        if (count >= 0 && emitted >= count) break;
        size_t member_len;
        const char *member = zvItemMember(item, &member_len);
        addReplyBulkCBuffer(c, member, member_len);
        if (withscores) {
            sds formatted = zvItemFormatScore(item);
            addReplyBulkSds(c, formatted);
        }
        emitted++;
    }
    setDeferredArrayLen(c, arraylen_ptr, withscores ? emitted * 2 : emitted);
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
                size_t mlen;
                const char *member = zvItemMember(next, &mlen);
                /* Borrowed member slice as a lookup key: copy to a
                 * temporary SDS for the marking protocol. */
                sds tmp = sdsnewlen(member, mlen);
                int present = 1;
                for (long i = 0; i < setnum; i++) {
                    if (i == driver) continue;
                    if (src[i].subject == NULL) {
                        present = 0;
                        break;
                    }
                    if (zvsetFind(objectGetVal(src[i].subject), tmp) == NULL) {
                        present = 0;
                        break;
                    }
                }
                sdsfree(tmp);
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
                sds mtmp = sdsnewlen(member, mlen);
                zsetMarkLookupKey(mtmp);
                void **slot = hashtableFindRef(tmp->ht, mtmp);
                zsetUnmarkLookupKey(mtmp);
                if (slot == NULL) {
                    sds item = zvItemCreate(scratch_b, member, mlen);
                    sds inserted = fbtreeInsert(tmp->tree, item);
                    serverAssert(hashtableAdd(tmp->ht, inserted));
                } else {
                    zvItemToScore(*slot, scratch_a);
                    zvScoreAggregate(scratch_a, scratch_b, aggregate);
                    if (!zvItemScoreEquals(*slot, scratch_a)) {
                        const_sds old = *slot;
                        sds item = zvItemCreate(scratch_a, member, mlen);
                        serverAssert(fbtreeDelete(tmp->tree, old));
                        *slot = fbtreeInsert(tmp->tree, item);
                    }
                }
                sdsfree(mtmp);
            }
            hashtableCleanupIterator(&iter);
        }
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
                sds mtmp = sdsnewlen(member, mlen);
                int k;
                for (k = 0; k < setnum; k++) {
                    if (k == driver) continue;
                    void *found = zvsetFind(objectGetVal(src[k].subject), mtmp);
                    if (found == NULL) break;
                    zvItemToScore(found, scratch_a);
                    zvScoreApplyWeight(scratch_b, scratch_a, src[k].weight);
                    zvScoreAggregate(scratch_c, scratch_b, aggregate);
                }
                sdsfree(mtmp);
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
                size_t mlen;
                const char *member = zvItemMember(next, &mlen);
                sds mtmp = sdsnewlen(member, mlen);
                int k;
                for (k = 1; k < setnum; k++) {
                    if (src[k].subject == NULL) continue;
                    if (zvsetFind(objectGetVal(src[k].subject), mtmp) != NULL) break;
                }
                sdsfree(mtmp);
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
            notifyKeyspaceEvent(NOTIFY_GENERIC, (char *)opnames[op], dstkey, c->db->id);
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
            if (withscores && c->resp > 2) addReplyArrayLen(c, 2);
            size_t mlen;
            const char *member = zvItemMember(item, &mlen);
            addReplyBulkCBuffer(c, member, mlen);
            if (withscores) {
                sds formatted = zvItemFormatScore(item);
                addReplyBulkSds(c, formatted);
            }
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

void zvcountCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;

    if (c->argc != 4) {
        addReplyErrorObject(c, shared.syntaxerr);
        return;
    }
    sds minraw = objectGetVal(c->argv[2]);
    sds maxraw = objectGetVal(c->argv[3]);
    if (!zvScoreBoundSyntaxOk(minraw, sdslen(minraw)) || !zvScoreBoundSyntaxOk(maxraw, sdslen(maxraw))) {
        addReplyError(c, "invalid vector score range");
        return;
    }
    if ((zobj = lookupKeyReadOrReply(c, key, shared.czero)) == NULL || checkType(c, zobj, OBJ_ZVSET)) return;
    zvset *zs = objectGetVal(zobj);
    zvScoreBound minb = {0}, maxb = {0};
    if (zvParseScoreBound(minraw, sdslen(minraw), zs->dimensions, &minb) != C_OK ||
        zvParseScoreBound(maxraw, sdslen(maxraw), zs->dimensions, &maxb) != C_OK) {
        zvFreeScoreBound(&minb);
        zvFreeScoreBound(&maxb);
        addReplyError(c, "invalid vector score range");
        return;
    }
    sds lower = NULL, upper = NULL;
    int empty = 0;
    zvBuildScoreInterval(&minb, &maxb, &lower, &upper, &empty);
    zvFreeScoreBound(&minb);
    zvFreeScoreBound(&maxb);
    long long count;
    if (empty) {
        count = 0;
    } else if (lower != NULL && upper != NULL) {
        count = (long long)fbtreeCountRangeByValue(zs->tree, lower, upper, 0, 1);
    } else {
        unsigned long lo, hi;
        zvScoreRanks(zs, lower, upper, &lo, &hi);
        count = (long long)(hi - lo);
    }
    if (lower) sdsfree(lower);
    if (upper) sdsfree(upper);
    addReplyLongLong(c, count);
}

void zvcardCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;

    if ((zobj = lookupKeyReadOrReply(c, key, shared.czero)) == NULL || checkType(c, zobj, OBJ_ZVSET))
        return;
    zvset *zs = objectGetVal(zobj);
    /* Debug builds: ht and tree must agree. */
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
        sds formatted = zvItemFormatScore(item);
        size_t member_len;
        const char *member = zvItemMember(item, &member_len);
        int ok = rioWriteBulkString(r, formatted, sdslen(formatted)) && rioWriteBulkString(r, member, member_len);
        sdsfree(formatted);
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
