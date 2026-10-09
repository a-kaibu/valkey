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

void zvrangeCommand(client *c) {
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

    zvset *zs = objectGetVal(zobj);
    unsigned long len = zvsetLength(zs);

    /* Normalize negative indexes like ZRANGE. */
    if (start < 0) start = (long)len + start;
    if (stop < 0) stop = (long)len + stop;
    if (start < 0) start = 0;
    if (stop < 0 || start > stop || start >= (long)len) {
        addReplyArrayLen(c, 0);
        return;
    }
    if (stop >= (long)len) stop = (long)len - 1;
    long rangelen = stop - start + 1;

    if (withscores)
        addReplyArrayLen(c, rangelen * 2);
    else
        addReplyArrayLen(c, rangelen);

    fbtreeIterator iter;
    fbtreeInitIterator(&iter, zs->tree);
    if (start > 0) fbtreeSeekToRank(&iter, (unsigned long)start);
    for (long i = 0; i < rangelen; i++) {
        const_sds item = fbtreeNext(&iter);
        serverAssertWithInfo(c, zobj, item != NULL);
        size_t member_len;
        const char *member = zvItemMember(item, &member_len);
        addReplyBulkCBuffer(c, member, member_len);
        if (withscores) {
            sds formatted = zvItemFormatScore(item);
            addReplyBulkSds(c, formatted);
        }
    }
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
