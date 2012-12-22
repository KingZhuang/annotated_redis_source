/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "redis.h"

void signalListAsReady(redisClient *c, robj *key);

/*-----------------------------------------------------------------------------
 * List API
 *----------------------------------------------------------------------------*/

/* Check the argument length to see if it requires us to convert the ziplist
 * to a real list. Only check raw-encoded objects because integer encoded
 * objects are never too long. */
/*
 * 检查 value ，如果它是一个字符串的话，看看 ziplist 能否满足储存它的长度要求
 * 如果不能的话，将 subject 转换为双端链表
 *
 * 如果 value 为整数类型，那么不必对它检查，因为整数对象最长只能是 long 类型
 */
void listTypeTryConversion(robj *subject, robj *value) {

    // 已经是 LINKEDLIST
    if (subject->encoding != REDIS_ENCODING_ZIPLIST) return;

    if (value->encoding == REDIS_ENCODING_RAW &&
        sdslen(value->ptr) > server.list_max_ziplist_value)
            listTypeConvert(subject,REDIS_ENCODING_LINKEDLIST);
}

/* The function pushes an elmenet to the specified list object 'subject',
 * at head or tail position as specified by 'where'.
 *
 * There is no need for the caller to incremnet the refcount of 'value' as
 * the function takes care of it if needed. */
/*
 * 多态推入函数
 *
 * 根据 where 参数，将 value 推入列表 subject 的表头或表尾
 *
 * 调用者不必对 value 进行计数，这个函数会处理它
 */
void listTypePush(robj *subject, robj *value, int where) {
    /* Check if we need to convert the ziplist */
    // 检查是否需要对列表进行编码转换
    listTypeTryConversion(subject,value);
    if (subject->encoding == REDIS_ENCODING_ZIPLIST &&
        ziplistLen(subject->ptr) >= server.list_max_ziplist_entries)
            listTypeConvert(subject,REDIS_ENCODING_LINKEDLIST);

    // ziplist
    if (subject->encoding == REDIS_ENCODING_ZIPLIST) {
        int pos = (where == REDIS_HEAD) ? ZIPLIST_HEAD : ZIPLIST_TAIL;
        value = getDecodedObject(value);
        subject->ptr = ziplistPush(subject->ptr,value->ptr,sdslen(value->ptr),pos);
        decrRefCount(value);
    // 双端链表
    } else if (subject->encoding == REDIS_ENCODING_LINKEDLIST) {
        if (where == REDIS_HEAD) {
            listAddNodeHead(subject->ptr,value);
        } else {
            listAddNodeTail(subject->ptr,value);
        }
        incrRefCount(value);
    } else {
        redisPanic("Unknown list encoding");
    }
}

/*
 * 多态 pop 对象
 */
robj *listTypePop(robj *subject, int where) {

    robj *value = NULL;

    if (subject->encoding == REDIS_ENCODING_ZIPLIST) {
        // 从 ziplist 中 pop
        unsigned char *p;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;
        // pop 表头或表尾？
        int pos = (where == REDIS_HEAD) ? 0 : -1;
        p = ziplistIndex(subject->ptr,pos);
        // 元素获取成功？
        if (ziplistGet(p,&vstr,&vlen,&vlong)) {
            // 取出值
            if (vstr) {
                value = createStringObject((char*)vstr,vlen);
            } else {
                value = createStringObjectFromLongLong(vlong);
            }
            /* We only need to delete an element when it exists */
            // 删除它
            subject->ptr = ziplistDelete(subject->ptr,&p);
        }
    } else if (subject->encoding == REDIS_ENCODING_LINKEDLIST) {
        // 从双端列表中 pop
        list *list = subject->ptr;
        listNode *ln;
        // 取出表头节点
        if (where == REDIS_HEAD) {
            ln = listFirst(list);
        // 取出表尾节点
        } else {
            ln = listLast(list);
        }

        if (ln != NULL) {
            // 取出节点的值
            value = listNodeValue(ln);

            incrRefCount(value);
            
            // 删除节点
            listDelNode(list,ln);
        }
    } else {
        redisPanic("Unknown list encoding");
    }

    return value;
}

/*
 * 多态列表长度获取函数
 */
unsigned long listTypeLength(robj *subject) {
    if (subject->encoding == REDIS_ENCODING_ZIPLIST) {
        return ziplistLen(subject->ptr);
    } else if (subject->encoding == REDIS_ENCODING_LINKEDLIST) {
        return listLength((list*)subject->ptr);
    } else {
        redisPanic("Unknown list encoding");
    }
}

/* Initialize an iterator at the specified index. */
/*
 * 创建多态列表迭代器
 */
listTypeIterator *listTypeInitIterator(robj *subject, long index, unsigned char direction) {

    listTypeIterator *li = zmalloc(sizeof(listTypeIterator));

    li->subject = subject;
    li->encoding = subject->encoding;
    li->direction = direction;

    if (li->encoding == REDIS_ENCODING_ZIPLIST) {
        li->zi = ziplistIndex(subject->ptr,index);
    } else if (li->encoding == REDIS_ENCODING_LINKEDLIST) {
        li->ln = listIndex(subject->ptr,index);
    } else {
        redisPanic("Unknown list encoding");
    }
    return li;
}

/* Clean up the iterator. */
void listTypeReleaseIterator(listTypeIterator *li) {
    zfree(li);
}

/* Stores pointer to current the entry in the provided entry structure
 * and advances the position of the iterator. Returns 1 when the current
 * entry is in fact an entry, 0 otherwise. */
/*
 * 将当前迭代到的节点保存到 entry ，并将迭代器的指针向前推移一步。
 *
 * 获取节点成功返回 1 ，否则返回 0 。
 */
int listTypeNext(listTypeIterator *li, listTypeEntry *entry) {
    /* Protect from converting when iterating */
    redisAssert(li->subject->encoding == li->encoding);

    entry->li = li;

    if (li->encoding == REDIS_ENCODING_ZIPLIST) {
        entry->zi = li->zi;
        if (entry->zi != NULL) {
            if (li->direction == REDIS_TAIL)
                li->zi = ziplistNext(li->subject->ptr,li->zi);
            else
                li->zi = ziplistPrev(li->subject->ptr,li->zi);
            return 1;
        }
    } else if (li->encoding == REDIS_ENCODING_LINKEDLIST) {
        entry->ln = li->ln;
        if (entry->ln != NULL) {
            if (li->direction == REDIS_TAIL)
                li->ln = li->ln->next;
            else
                li->ln = li->ln->prev;
            return 1;
        }
    } else {
        redisPanic("Unknown list encoding");
    }

    return 0;
}

/* Return entry or NULL at the current position of the iterator. */
/*
 * 返回迭代器当前节点的值，如果迭代已经完成，返回 NULL 
 */
robj *listTypeGet(listTypeEntry *entry) {

    listTypeIterator *li = entry->li;

    robj *value = NULL;

    if (li->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;
        redisAssert(entry->zi != NULL);
        if (ziplistGet(entry->zi,&vstr,&vlen,&vlong)) {
            if (vstr) {
                value = createStringObject((char*)vstr,vlen);
            } else {
                value = createStringObjectFromLongLong(vlong);
            }
        }
    } else if (li->encoding == REDIS_ENCODING_LINKEDLIST) {
        redisAssert(entry->ln != NULL);
        value = listNodeValue(entry->ln);
        incrRefCount(value);
    } else {
        redisPanic("Unknown list encoding");
    }

    return value;
}

/*
 * 多态插入函数 
 */
void listTypeInsert(listTypeEntry *entry, robj *value, int where) {

    robj *subject = entry->li->subject;

    if (entry->li->encoding == REDIS_ENCODING_ZIPLIST) {
        value = getDecodedObject(value);
        if (where == REDIS_TAIL) {
            unsigned char *next = ziplistNext(subject->ptr,entry->zi);

            /* When we insert after the current element, but the current element
             * is the tail of the list, we need to do a push. */
            // 找到 next 就将节点插入在 next 之后，没找到就将节点放到表尾
            if (next == NULL) {
                subject->ptr = ziplistPush(subject->ptr,value->ptr,sdslen(value->ptr),REDIS_TAIL);
            } else {
                subject->ptr = ziplistInsert(subject->ptr,next,value->ptr,sdslen(value->ptr));
            }
        } else {
            subject->ptr = ziplistInsert(subject->ptr,entry->zi,value->ptr,sdslen(value->ptr));
        }
        decrRefCount(value);
    } else if (entry->li->encoding == REDIS_ENCODING_LINKEDLIST) {
        if (where == REDIS_TAIL) {
            listInsertNode(subject->ptr,entry->ln,value,AL_START_TAIL);
        } else {
            listInsertNode(subject->ptr,entry->ln,value,AL_START_HEAD);
        }
        incrRefCount(value);
    } else {
        redisPanic("Unknown list encoding");
    }
}

/* Compare the given object with the entry at the current position. */
/*
 * 对比 entry 的值和对象 o
 */
int listTypeEqual(listTypeEntry *entry, robj *o) {

    listTypeIterator *li = entry->li;

    if (li->encoding == REDIS_ENCODING_ZIPLIST) {
        redisAssertWithInfo(NULL,o,o->encoding == REDIS_ENCODING_RAW);
        return ziplistCompare(entry->zi,o->ptr,sdslen(o->ptr));
    } else if (li->encoding == REDIS_ENCODING_LINKEDLIST) {
        return equalStringObjects(o,listNodeValue(entry->ln));
    } else {
        redisPanic("Unknown list encoding");
    }
}

/* Delete the element pointed to. */
/*
 * 多态删除 entry 指向的元素
 */
void listTypeDelete(listTypeEntry *entry) {

    listTypeIterator *li = entry->li;

    if (li->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *p = entry->zi;
        li->subject->ptr = ziplistDelete(li->subject->ptr,&p);

        /* Update position of the iterator depending on the direction */
        if (li->direction == REDIS_TAIL)
            li->zi = p;
        else
            li->zi = ziplistPrev(li->subject->ptr,p);
    } else if (entry->li->encoding == REDIS_ENCODING_LINKEDLIST) {
        listNode *next;
        if (li->direction == REDIS_TAIL)
            next = entry->ln->next;
        else
            next = entry->ln->prev;
        listDelNode(li->subject->ptr,entry->ln);
        li->ln = next;
    } else {
        redisPanic("Unknown list encoding");
    }
}

/*
 * 将列表转换为给定的编码类型
 *
 * 目前只支持将 ziplist 转换为双端链表
 */
void listTypeConvert(robj *subject, int enc) {
    listTypeIterator *li;
    listTypeEntry entry;
    redisAssertWithInfo(NULL,subject,subject->type == REDIS_LIST);

    if (enc == REDIS_ENCODING_LINKEDLIST) {
        list *l = listCreate();
        listSetFreeMethod(l,decrRefCount);

        /* listTypeGet returns a robj with incremented refcount */
        li = listTypeInitIterator(subject,0,REDIS_TAIL);
        while (listTypeNext(li,&entry)) listAddNodeTail(l,listTypeGet(&entry));
        listTypeReleaseIterator(li);

        // 更新编码
        subject->encoding = REDIS_ENCODING_LINKEDLIST;
        // 释放 ziplist
        zfree(subject->ptr);
        // 指向双端链表
        subject->ptr = l;
    } else {
        redisPanic("Unsupported list conversion");
    }
}

/*-----------------------------------------------------------------------------
 * List Commands
 *----------------------------------------------------------------------------*/

void pushGenericCommand(redisClient *c, int where) {
    int j, waiting = 0, pushed = 0;
    // 查找列表对象
    robj *lobj = lookupKeyWrite(c->db,c->argv[1]);
    // 如果列表为空，那么可能正在有客户端等待这个列表
    int may_have_waiting_clients = (lobj == NULL);

    // 类型检查
    if (lobj && lobj->type != REDIS_LIST) {
        addReply(c,shared.wrongtypeerr);
        return;
    }

    // 检查是否有客户端在等待这个列表
    // 如果是的话，告知服务器和客户端，这个列表已经就绪
    if (may_have_waiting_clients) signalListAsReady(c,c->argv[1]);

    // 将所有输入元素推入列表
    for (j = 2; j < c->argc; j++) {
        c->argv[j] = tryObjectEncoding(c->argv[j]);
        if (!lobj) {
            lobj = createZiplistObject();
            dbAdd(c->db,c->argv[1],lobj);
        }
        listTypePush(lobj,c->argv[j],where);
        pushed++;
    }
    addReplyLongLong(c, waiting + (lobj ? listTypeLength(lobj) : 0));
    if (pushed) signalModifiedKey(c->db,c->argv[1]);
    server.dirty += pushed;
}

void lpushCommand(redisClient *c) {
    pushGenericCommand(c,REDIS_HEAD);
}

void rpushCommand(redisClient *c) {
    pushGenericCommand(c,REDIS_TAIL);
}

void pushxGenericCommand(redisClient *c, robj *refval, robj *val, int where) {
    robj *subject;
    listTypeIterator *iter;
    listTypeEntry entry;
    int inserted = 0;

    // 查找或创建，并做类型检查
    if ((subject = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,subject,REDIS_LIST)) return;

    // 命令指定 value 要放在 refval 之前或之后
    if (refval != NULL) {
        /* Note: we expect refval to be string-encoded because it is *not* the
         * last argument of the multi-bulk LINSERT. */
        redisAssertWithInfo(c,refval,refval->encoding == REDIS_ENCODING_RAW);

        /* We're not sure if this value can be inserted yet, but we cannot
         * convert the list inside the iterator. We don't want to loop over
         * the list twice (once to see if the value can be inserted and once
         * to do the actual insert), so we assume this value can be inserted
         * and convert the ziplist to a regular list if necessary. */
        // 检查添加 value 是否需要对 subject 进行编码转换
        listTypeTryConversion(subject,val);

        /* Seek refval from head to tail */
        // 从表头开始，向表尾查找包含 refval 的节点
        iter = listTypeInitIterator(subject,0,REDIS_TAIL);
        while (listTypeNext(iter,&entry)) {
            if (listTypeEqual(&entry,refval)) {
                // 找到，插入 val
                listTypeInsert(&entry,val,where);
                inserted = 1;
                break;
            }
        }
        listTypeReleaseIterator(iter);

        // value 已经插入成功？
        if (inserted) {
            /* Check if the length exceeds the ziplist length threshold. */
            // 检查是否需要对列表进行编码转换
            if (subject->encoding == REDIS_ENCODING_ZIPLIST &&
                ziplistLen(subject->ptr) > server.list_max_ziplist_entries)
                    listTypeConvert(subject,REDIS_ENCODING_LINKEDLIST);
            signalModifiedKey(c->db,c->argv[1]);
            server.dirty++;
        } else {
            /* Notify client of a failed insert */
            addReply(c,shared.cnegone);
            return;
        }
    } else {
        // 简单地将 value 推入到列表的之前或之后
        listTypePush(subject,val,where);

        signalModifiedKey(c->db,c->argv[1]);
        server.dirty++;
    }

    addReplyLongLong(c,listTypeLength(subject));
}

void lpushxCommand(redisClient *c) {
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    pushxGenericCommand(c,NULL,c->argv[2],REDIS_HEAD);
}

void rpushxCommand(redisClient *c) {
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    pushxGenericCommand(c,NULL,c->argv[2],REDIS_TAIL);
}

void linsertCommand(redisClient *c) {

    c->argv[4] = tryObjectEncoding(c->argv[4]);

    if (strcasecmp(c->argv[2]->ptr,"after") == 0) {
        pushxGenericCommand(c,c->argv[3],c->argv[4],REDIS_TAIL);
    } else if (strcasecmp(c->argv[2]->ptr,"before") == 0) {
        pushxGenericCommand(c,c->argv[3],c->argv[4],REDIS_HEAD);
    } else {
        addReply(c,shared.syntaxerr);
    }
}

void llenCommand(redisClient *c) {
    
    // 创建或查找列表对象
    robj *o = lookupKeyReadOrReply(c,c->argv[1],shared.czero);

    // 类型检查
    if (o == NULL || checkType(c,o,REDIS_LIST)) return;

    // 返回长度
    addReplyLongLong(c,listTypeLength(o));
}

void lindexCommand(redisClient *c) {
    robj *o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk);
    if (o == NULL || checkType(c,o,REDIS_LIST)) return;
    long index;
    robj *value = NULL;

    // 获取列表对象或返回不存在
    if ((getLongFromObjectOrReply(c, c->argv[2], &index, NULL) != REDIS_OK))
        return;

    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        // 从 ziplist 中获取
        unsigned char *p;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;
        p = ziplistIndex(o->ptr,index);
        if (ziplistGet(p,&vstr,&vlen,&vlong)) {
            // 取出值
            if (vstr) {
                value = createStringObject((char*)vstr,vlen);
            } else {
                value = createStringObjectFromLongLong(vlong);
            }
            addReplyBulk(c,value);
            decrRefCount(value);
        } else {
            addReply(c,shared.nullbulk);
        }
    } else if (o->encoding == REDIS_ENCODING_LINKEDLIST) {
        // 从双端列表中取出值
        listNode *ln = listIndex(o->ptr,index);
        if (ln != NULL) {
            value = listNodeValue(ln);
            addReplyBulk(c,value);
        } else {
            addReply(c,shared.nullbulk);
        }
    } else {
        redisPanic("Unknown list encoding");
    }
}

void lsetCommand(redisClient *c) {
    // 查找对象，或者返回不存在错误
    robj *o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr);

    // 类型检查
    if (o == NULL || checkType(c,o,REDIS_LIST)) return;

    long index;
    // 编码输入值
    robj *value = (c->argv[3] = tryObjectEncoding(c->argv[3]));

    // 获取 index 参数
    if ((getLongFromObjectOrReply(c, c->argv[2], &index, NULL) != REDIS_OK))
        return;

    // 如果有需要，转换列表的编码
    listTypeTryConversion(o,value);
    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        // 更新到 ziplist
        unsigned char *p, *zl = o->ptr;
        p = ziplistIndex(zl,index);
        if (p == NULL) {
            // index 越界
            addReply(c,shared.outofrangeerr);
        } else {
            // 先删除 ziplist 里指定 index 的值
            o->ptr = ziplistDelete(o->ptr,&p);
            // 再将新值添加到 ziplist 的末尾
            value = getDecodedObject(value);
            o->ptr = ziplistInsert(o->ptr,p,value->ptr,sdslen(value->ptr));

            decrRefCount(value);

            addReply(c,shared.ok);
            signalModifiedKey(c->db,c->argv[1]);
            server.dirty++;
        }
    } else if (o->encoding == REDIS_ENCODING_LINKEDLIST) {
        // 添加到双端链表
        listNode *ln = listIndex(o->ptr,index);
        if (ln == NULL) {
            addReply(c,shared.outofrangeerr);
        } else {
            // 删除 ln 原有的值
            decrRefCount((robj*)listNodeValue(ln));
            // 在用新值替换它
            listNodeValue(ln) = value;

            incrRefCount(value);

            addReply(c,shared.ok);
            signalModifiedKey(c->db,c->argv[1]);
            server.dirty++;
        }
    } else {
        redisPanic("Unknown list encoding");
    }
}

/*
 * 从列表中弹出一个元素，如果弹出元素之后列表为空，那么删除它
 */
void popGenericCommand(redisClient *c, int where) {
    // 查找对象，或者返回不存在信息
    robj *o = lookupKeyWriteOrReply(c,c->argv[1],shared.nullbulk);

    // 类型检查
    if (o == NULL || checkType(c,o,REDIS_LIST)) return;

    // 调用多态 pop 函数
    robj *value = listTypePop(o,where);
    if (value == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        addReplyBulk(c,value);

        decrRefCount(value);

        // 如果列表为空，那么删除它
        if (listTypeLength(o) == 0) dbDelete(c->db,c->argv[1]);

        signalModifiedKey(c->db,c->argv[1]);
        server.dirty++;
    }
}

void lpopCommand(redisClient *c) {
    popGenericCommand(c,REDIS_HEAD);
}

void rpopCommand(redisClient *c) {
    popGenericCommand(c,REDIS_TAIL);
}

void lrangeCommand(redisClient *c) {
    robj *o;
    long start, end, llen, rangelen;

    // 检查 start 和 end 参数是否整数
    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != REDIS_OK) ||
        (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != REDIS_OK)) return;

    // 查找对象，对象不存在时返回空
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptymultibulk)) == NULL
         || checkType(c,o,REDIS_LIST)) return;

    llen = listTypeLength(o);

    /* convert negative indexes */
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    if (start > end || start >= llen) {
        addReply(c,shared.emptymultibulk);
        return;
    }
    if (end >= llen) end = llen-1;
    rangelen = (end-start)+1;

    /* Return the result in form of a multi-bulk reply */
    addReplyMultiBulkLen(c,rangelen);
    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        // 处理 ziplist
        unsigned char *p = ziplistIndex(o->ptr,start);
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;

        while(rangelen--) {
            ziplistGet(p,&vstr,&vlen,&vlong);
            if (vstr) {
                addReplyBulkCBuffer(c,vstr,vlen);
            } else {
                addReplyBulkLongLong(c,vlong);
            }
            p = ziplistNext(o->ptr,p);
        }
    } else if (o->encoding == REDIS_ENCODING_LINKEDLIST) {
        // 处理双端链表
        listNode *ln;

        /* If we are nearest to the end of the list, reach the element
         * starting from tail and going backward, as it is faster. */
        if (start > llen/2) start -= llen;
        ln = listIndex(o->ptr,start);

        while(rangelen--) {
            addReplyBulk(c,ln->value);
            ln = ln->next;
        }
    } else {
        redisPanic("List encoding is not LINKEDLIST nor ZIPLIST!");
    }
}

void ltrimCommand(redisClient *c) {
    robj *o;
    long start, end, llen, j, ltrim, rtrim;
    list *list;
    listNode *ln;

    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != REDIS_OK) ||
        (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != REDIS_OK)) return;

    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.ok)) == NULL ||
        checkType(c,o,REDIS_LIST)) return;
    llen = listTypeLength(o);

    /* convert negative indexes */
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    if (start > end || start >= llen) {
        /* Out of range start or start > end result in empty list */
        ltrim = llen;
        rtrim = 0;
    } else {
        if (end >= llen) end = llen-1;
        ltrim = start;
        rtrim = llen-end-1;
    }

    /* Remove list elements to perform the trim */
    // 删除
    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        o->ptr = ziplistDeleteRange(o->ptr,0,ltrim);
        o->ptr = ziplistDeleteRange(o->ptr,-rtrim,rtrim);
    } else if (o->encoding == REDIS_ENCODING_LINKEDLIST) {
        list = o->ptr;
        // 从表头向表尾删除
        for (j = 0; j < ltrim; j++) {
            ln = listFirst(list);
            listDelNode(list,ln);
        }
        // 从表尾向表头删除
        for (j = 0; j < rtrim; j++) {
            ln = listLast(list);
            listDelNode(list,ln);
        }
    } else {
        redisPanic("Unknown list encoding");
    }

    // 列表为空？删除它
    if (listTypeLength(o) == 0) dbDelete(c->db,c->argv[1]);

    signalModifiedKey(c->db,c->argv[1]);
    server.dirty++;
    addReply(c,shared.ok);
}

void lremCommand(redisClient *c) {
    robj *subject, *obj;

    // obj 是要删除的目标对象
    obj = c->argv[3] = tryObjectEncoding(c->argv[3]);

    long toremove;
    long removed = 0;
    listTypeEntry entry;

    // toremove 决定删除值的方式
    if ((getLongFromObjectOrReply(c, c->argv[2], &toremove, NULL) != REDIS_OK))
        return;

    // 获取对象，或者返回空值
    subject = lookupKeyWriteOrReply(c,c->argv[1],shared.czero);
    // 类型检查
    if (subject == NULL || checkType(c,subject,REDIS_LIST)) return;

    /* Make sure obj is raw when we're dealing with a ziplist */
    if (subject->encoding == REDIS_ENCODING_ZIPLIST)
        obj = getDecodedObject(obj);

    // 根据 toremove ，决定是迭代器遍历的方式（从头到尾或者从尾到头）
    listTypeIterator *li;
    if (toremove < 0) {
        // 修改为绝对值
        toremove = -toremove;
        li = listTypeInitIterator(subject,-1,REDIS_HEAD);
    } else {
        li = listTypeInitIterator(subject,0,REDIS_TAIL);
    }

    // 遍历并删除
    while (listTypeNext(li,&entry)) {
        if (listTypeEqual(&entry,obj)) {
            listTypeDelete(&entry);
            server.dirty++;
            removed++;
            if (toremove && removed == toremove) break;
        }
    }
    listTypeReleaseIterator(li);

    /* Clean up raw encoded object */
    if (subject->encoding == REDIS_ENCODING_ZIPLIST)
        decrRefCount(obj);

    // 列表为空？删除它
    if (listTypeLength(subject) == 0) dbDelete(c->db,c->argv[1]);

    addReplyLongLong(c,removed);
    if (removed) signalModifiedKey(c->db,c->argv[1]);
}

/* This is the semantic of this command:
 *  RPOPLPUSH srclist dstlist:
 *    IF LLEN(srclist) > 0
 *      element = RPOP srclist
 *      LPUSH dstlist element
 *      RETURN element
 *    ELSE
 *      RETURN nil
 *    END
 *  END
 *
 * The idea is to be able to get an element from a list in a reliable way
 * since the element is not just returned but pushed against another list
 * as well. This command was originally proposed by Ezra Zygmuntowicz.
 */

/*
 * 将 value 添加到 dstkey 列表里
 * 如果 dstkey 为空，那么创建一个新列表，然后执行添加动作
 */
void rpoplpushHandlePush(redisClient *c, robj *dstkey, robj *dstobj, robj *value) {
    /* Create the list if the key does not exist */
    // 列表不存在，创建列表
    if (!dstobj) {
        // 创建 ziplist
        dstobj = createZiplistObject();
        // 添加到 db
        dbAdd(c->db,dstkey,dstobj);
        // 将 dstkey 添加到 server.ready_keys 列表里
        signalListAsReady(c,dstkey);
    }

    signalModifiedKey(c->db,dstkey);

    // 添加 value 到 dstobj
    listTypePush(dstobj,value,REDIS_HEAD);

    /* Always send the pushed value to the client. */
    addReplyBulk(c,value);
}

void rpoplpushCommand(redisClient *c) {
    robj *sobj, *value;

    // 获取源对象，对象不存在则返回 NULL
    if ((sobj = lookupKeyWriteOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,sobj,REDIS_LIST)) return;

    // 列表为空？
    if (listTypeLength(sobj) == 0) {
        /* This may only happen after loading very old RDB files. Recent
         * versions of Redis delete keys of empty lists. */
        addReply(c,shared.nullbulk);
    } else {
        // 列表非空

        // 获取目标对象
        robj *dobj = lookupKeyWrite(c->db,c->argv[2]);
        // 源对象的 key
        robj *touchedkey = c->argv[1];

        // 对目标对象进行类型检查
        if (dobj && checkType(c,dobj,REDIS_LIST)) return;

        // 弹出目标对象的表尾值
        value = listTypePop(sobj,REDIS_TAIL);
        /* We saved touched key, and protect it, since rpoplpushHandlePush
         * may change the client command argument vector (it does not
         * currently). */
        incrRefCount(touchedkey);
        rpoplpushHandlePush(c,c->argv[2],dobj,value);

        /* listTypePop returns an object with its refcount incremented */
        decrRefCount(value);

        /* Delete the source list when it is empty */
        if (listTypeLength(sobj) == 0) dbDelete(c->db,touchedkey);

        signalModifiedKey(c->db,touchedkey);
        decrRefCount(touchedkey);
        server.dirty++;
    }
}

/*-----------------------------------------------------------------------------
 * Blocking POP operations
 *----------------------------------------------------------------------------*/

/* This is how the current blocking POP works, we use BLPOP as example:
 * 以下是阻塞弹出的相关原理，用 BLPOP 为例子：
 *
 * - If the user calls BLPOP and the key exists and contains a non empty list
 *   then LPOP is called instead. So BLPOP is semantically the same as LPOP
 *   if blocking is not required.
 * - 如果 BLPOP 被调用，并且给定 key 不为空，那么直接调用 POP 。
 *
 * - If instead BLPOP is called and the key does not exists or the list is
 *   empty we need to block. In order to do so we remove the notification for
 *   new data to read in the client socket (so that we'll not serve new
 *   requests if the blocking request is not served). Also we put the client
 *   in a dictionary (db->blocking_keys) mapping keys to a list of clients
 *   blocking for this keys.
 * - 如果 BLPOP 被调用，且 key 不存在或列表为空，那么对客户端进行阻塞。
 *   在对客户端进行阻塞时，只有在有新数据可读的情况下，才向客户端发送通知，
 *   （这样就可以在没有数据数据时，不对阻塞客户端进行处理）。
 *   另外还将一个 client 到 key 的映射添加到由阻塞 key 组成的链表里面，
 *   这个链表按 key 为键，保存在字典 db->blocking_keys 。
 *
 * - If a PUSH operation against a key with blocked clients waiting is
 *   performed, we mark this key as "ready", and after the current command,
 *   MULTI/EXEC block, or script, is executed, we serve all the clients waiting
 *   for this list, from the one that blocked first, to the last, accordingly
 *   to the number of elements we have in the ready list.
 *   一旦某个造成客户端阻塞的 key 接受了 PUSH 操作，
 *   那么将这个 key 标记为『就绪』，并在这个命令/事务/脚本执行完之后，
 *   按先阻塞先服务的顺序，处理所有因这个 key 而被阻塞的客户端。
 */

/* Set a client in blocking mode for the specified key, with the specified
 * timeout */
/*
 * 根据给定 key ，对给定客户端进行阻塞
 */
void blockForKeys(redisClient *c, robj **keys, int numkeys, time_t timeout, robj *target) {
    dictEntry *de;
    list *l;
    int j;

    c->bpop.timeout = timeout;
    c->bpop.target = target;

    if (target != NULL) incrRefCount(target);

    for (j = 0; j < numkeys; j++) {
        /* If the key already exists in the dict ignore it. */
        // 记录阻塞 key 到客户端
        if (dictAdd(c->bpop.keys,keys[j],NULL) != DICT_OK) continue;
        incrRefCount(keys[j]);

        /* And in the other "side", to map keys -> clients */
        // 记录阻塞 key 到 db
        de = dictFind(c->db->blocking_keys,keys[j]);
        if (de == NULL) {
            // 这个 key 第一次被阻塞，创建一个链表
            int retval;

            /* For every key we take a list of clients blocked for it */
            l = listCreate();
            retval = dictAdd(c->db->blocking_keys,keys[j],l);
            incrRefCount(keys[j]);
            redisAssertWithInfo(c,keys[j],retval == DICT_OK);
        } else {
            // 已经有其他客户端被这个 key 阻塞 
            l = dictGetVal(de);
        }
        listAddNodeTail(l,c);
    }

    /* Mark the client as a blocked client */
    c->flags |= REDIS_BLOCKED;

    server.bpop_blocked_clients++;
}

/* Unblock a client that's waiting in a blocking operation such as BLPOP */
/*
 * 取消客户端的阻塞状态
 */
void unblockClientWaitingData(redisClient *c) {
    dictEntry *de;
    dictIterator *di;
    list *l;

    redisAssertWithInfo(c,NULL,dictSize(c->bpop.keys) != 0);

    // 遍历所有 key ，将它们从客户端 db->blocking_keys 的链表中移除
    di = dictGetIterator(c->bpop.keys);
    /* The client may wait for multiple keys, so unblock it for every key. */
    while((de = dictNext(di)) != NULL) {
        robj *key = dictGetKey(de);

        /* Remove this client from the list of clients waiting for this key. */
        // 获取阻塞 key 的所有客户端链表
        l = dictFetchValue(c->db->blocking_keys,key);
        redisAssertWithInfo(c,key,l != NULL);
        // 将本客户端从该链表中移除
        listDelNode(l,listSearchKey(l,c));
        /* If the list is empty we need to remove it to avoid wasting memory */
        // 如果没有其他客户端阻塞在这个 key 上，那么删除这个链表
        if (listLength(l) == 0)
            dictDelete(c->db->blocking_keys,key);
    }
    dictReleaseIterator(di);

    /* Cleanup the client structure */
    dictEmpty(c->bpop.keys);
    if (c->bpop.target) {
        decrRefCount(c->bpop.target);
        c->bpop.target = NULL;
    }

    c->flags &= ~REDIS_BLOCKED;
    c->flags |= REDIS_UNBLOCKED;

    server.bpop_blocked_clients--;

    // 输出回复到该客户端（如果有的话）
    listAddNodeTail(server.unblocked_clients,c);
}

/* If the specified key has clients blocked waiting for list pushes, this
 * function will put the key reference into the server.ready_keys list.
 * Note that db->ready_keys is an hash table that allows us to avoid putting
 * the same key agains and again in the list in case of multiple pushes
 * made by a script or in the context of MULTI/EXEC.
 *
 * The list will be finally processed by handleClientsBlockedOnLists() */
// 如果有客户端正因为等待给定 key 被 push 而阻塞，
// 那么将这个 key 的引用放进 server.ready_keys 列表里面。
//
// 注意 db->ready_keys 是一个哈希表，
// 这可以避免在事务或者脚本中，将同一个 key 一次又一次添加到列表的情况出现。
//
// 列表最终会被 handleClientsBlockedOnLists() 函数处理
void signalListAsReady(redisClient *c, robj *key) {
    readyList *rl;

    /* No clients blocking for this key? No need to queue it. */
    // 没有客户端在等待这个 key ，直接返回
    if (dictFind(c->db->blocking_keys,key) == NULL) return;

    /* Key was already signaled? No need to queue it again. */
    // key 已经位于就绪列表，直接返回
    if (dictFind(c->db->ready_keys,key) != NULL) return;

    /* Ok, we need to queue this key into server.ready_keys. */
    // 添加包含 key 及其 db 信息的 readyList 结构到服务器端的就绪列表
    rl = zmalloc(sizeof(*rl));
    rl->key = key;
    rl->db = c->db;
    incrRefCount(key);
    listAddNodeTail(server.ready_keys,rl);

    /* We also add the key in the db->ready_keys dictionary in order
     * to avoid adding it multiple times into a list with a simple O(1)
     * check. */
    // 同时将 key 添加到 db 的 ready_keys 字典中
    // 提供 O(1) 复杂度来查询某个 key 是否已经就绪
    incrRefCount(key);
    redisAssert(dictAdd(c->db->ready_keys,key,NULL) == DICT_OK);
}

/* This is an helper function for handleClientsBlockedOnLists(). It's work
 * is to serve a specific client (receiver) that is blocked on 'key'
 * in the context of the specified 'db', doing the following:
 *
 * 函数对被阻塞的客户端 receiver 、造成阻塞的 key 、 key 所在的数据库 db
 * 以及一个值 value 和一个位置值 where 执行以下动作：
 *
 * 1) Provide the client with the 'value' element.
 *    将 value 提供给 receiver
 * 2) If the dstkey is not NULL (we are serving a BRPOPLPUSH) also push the
 *    'value' element on the destionation list (the LPUSH side of the command).
 *    如果 dstkey 不为空（BRPOPLPUSH的情况），
 *    那么也将 value 推入到 dstkey 指定的列表中。
 * 3) Propagate the resulting BRPOP, BLPOP and additional LPUSH if any into
 *    the AOF and replication channel.
 *    将 BRPOP 、 BLPOP 和可能有的 LPUSH 传播到 AOF 和同步节点
 *
 * The argument 'where' is REDIS_TAIL or REDIS_HEAD, and indicates if the
 * 'value' element was popped fron the head (BLPOP) or tail (BRPOP) so that
 * we can propagate the command properly.
 * where 可能是 REDIS_TAIL 或者 REDIS_HEAD ，用于识别该 value 是从那个地方 POP
 * 出来，依靠这个参数，可以同样传播 BLPOP 或者 BRPOP 。
 *
 * The function returns REDIS_OK if we are able to serve the client, otherwise
 * REDIS_ERR is returned to signal the caller that the list POP operation
 * should be undoed as the client was not served: This only happens for
 * BRPOPLPUSH that fails to push the value to the destination key as it is
 * of the wrong type. 
 * 如果一切成功，返回 REDIS_OK 。
 * 如果执行失败，那么返回 REDIS_ERR ，让 Redis 撤销对目标节点的 POP 操作。
 * 失败的情况只会出现在 BRPOPLPUSH 命令中，
 * 比如 POP 列表成功，却发现想 PUSH 的目标不是列表时。
 */
int serveClientBlockedOnList(redisClient *receiver, robj *key, robj *dstkey, redisDb *db, robj *value, int where)
{
    robj *argv[3];

    // 不是 BLPOPRPUSH ？
    if (dstkey == NULL) {
        /* Propagate the [LR]POP operation. */
        // 传播 [LR]POP 操作
        argv[0] = (where == REDIS_HEAD) ? shared.lpop :
                                          shared.rpop;
        argv[1] = key;
        propagate((where == REDIS_HEAD) ?
            server.lpopCommand : server.rpopCommand,
            db->id,argv,2,REDIS_PROPAGATE_AOF|REDIS_PROPAGATE_REPL);

        /* BRPOP/BLPOP */
        // 回复客户端
        addReplyMultiBulkLen(receiver,2);
        addReplyBulk(receiver,key);     // 弹出 value 的 key
        addReplyBulk(receiver,value);   // value
    } else {
        /* BRPOPLPUSH */

        // 获取 dstkey 对象
        robj *dstobj = lookupKeyWrite(receiver->db,dstkey);

        // 对象不为空，且类型正确？
        if (!(dstobj &&
             checkType(receiver,dstobj,REDIS_LIST)))
        {
            /* Propagate the RPOP operation. */
            // 传播 RPOP 操作
            argv[0] = shared.rpop;
            argv[1] = key;
            propagate(server.rpopCommand,
                db->id,argv,2,
                REDIS_PROPAGATE_AOF|
                REDIS_PROPAGATE_REPL);
            // 将 value 添加到 dstkey 列表里
            // 如果 dstkey 不存在，那么创建一个新列表，
            // 然后进行添加操作
            rpoplpushHandlePush(receiver,dstkey,dstobj,
                value);
            /* Propagate the LPUSH operation. */
            // 传播 LPUSH 操作
            argv[0] = shared.lpush;
            argv[1] = dstkey;
            argv[2] = value;
            propagate(server.lpushCommand,
                db->id,argv,3,
                REDIS_PROPAGATE_AOF|
                REDIS_PROPAGATE_REPL);
        } else {
            /* BRPOPLPUSH failed because of wrong
             * destination type. */
            return REDIS_ERR;
        }
    }
    return REDIS_OK;
}

/* This function should be called by Redis every time a single command,
 * a MULTI/EXEC block, or a Lua script, terminated its execution after
 * being called by a client.
 *
 * 这个函数会在每次客户端执行单个命令/事务/脚本结束之后被调用。
 *
 * All the keys with at least one client blocked that received at least
 * one new element via some PUSH operation are accumulated into
 * the server.ready_keys list. This function will run the list and will
 * serve clients accordingly. Note that the function will iterate again and
 * again as a result of serving BRPOPLPUSH we can have new blocking clients
 * to serve because of the PUSH side of BRPOPLPUSH. 
 *
 * 对所有被阻塞在某个客户端的 key 来说，只要这个 key 被执行了某种 PUSH 操作
 * 那么这个 key 就会被放到 serve.ready_keys 去。
 * 
 * 这个函数会遍历整个 serve.ready_keys 链表，并对里面的 key 进行处理。
 *
 * 函数会一次又一次地进行迭代，
 * 因此它在执行 BRPOPLPUSH 命令的情况下也可以正常获取到正确的新被阻塞客户端。
 */
void handleClientsBlockedOnLists(void) {
    while(listLength(server.ready_keys) != 0) {
        list *l;

        /* Point server.ready_keys to a fresh list and save the current one
         * locally. This way as we run the old list we are free to call
         * signalListAsReady() that may push new elements in server.ready_keys
         * when handling clients blocked into BRPOPLPUSH. */
        // 备份旧的 ready_keys ，再给服务器端赋值一个新的
        l = server.ready_keys;
        server.ready_keys = listCreate();

        // 遍历整个链表
        while(listLength(l) != 0) {
            // 获取头节点
            listNode *ln = listFirst(l);
            // 获取元素的值
            readyList *rl = ln->value;

            /* First of all remove this key from db->ready_keys so that
             * we can safely call signalListAsReady() against this key. */
            // 从 db->ready_keys 中删除给定 key
            dictDelete(rl->db->ready_keys,rl->key);

            /* If the key exists and it's a list, serve blocked clients
             * with data. */
            // 获取 key 对象
            robj *o = lookupKeyWrite(rl->db,rl->key);
            // 对象不为空且是列表
            if (o != NULL && o->type == REDIS_LIST) {
                dictEntry *de;

                /* We serve clients in the same order they blocked for
                 * this key, from the first blocked to the last. */
                // 取出链表中包含的所有被给定 key 阻塞的客户端
                de = dictFind(rl->db->blocking_keys,rl->key);
                if (de) {
                    list *clients = dictGetVal(de);
                    int numclients = listLength(clients);

                    while(numclients--) {
                        // 取出客户端
                        listNode *clientnode = listFirst(clients);
                        redisClient *receiver = clientnode->value;

                        // 弹出的目标（只用于 BRPOPLPUSH）
                        robj *dstkey = receiver->bpop.target;

                        // 要弹出元素的位置
                        int where = (receiver->lastcmd &&
                                     receiver->lastcmd->proc == blpopCommand) ?
                                    REDIS_HEAD : REDIS_TAIL;

                        // 被弹出的值
                        robj *value = listTypePop(o,where);

                        if (value) {
                            /* Protect receiver->bpop.target, that will be
                             * freed by the next unblockClientWaitingData()
                             * call. */
                            if (dstkey) incrRefCount(dstkey);

                            // 取消 receiver 客户端的阻塞状态
                            unblockClientWaitingData(receiver);

                            // 将值 value 添加到
                            // 造成客户端 receiver 阻塞的 key 上
                            if (serveClientBlockedOnList(
                                receiver,   // 被阻塞的客户端
                                rl->key,    // 造成阻塞的 key
                                dstkey,     // 目标 key （BRPOPLPUSH）
                                rl->db,     // 数据库
                                value,      // 值
                                where) == REDIS_ERR)
                            {
                                /* If we failed serving the client we need
                                 * to also undo the POP operation. */
                                // 如果处理失败，需要重新将 POP 出来的
                                // 元素 PUSH 回去
                                    listTypePush(o,value,where);
                            }

                            if (dstkey) decrRefCount(dstkey);
                            decrRefCount(value);
                        } else {
                            break;
                        }
                    }
                }
               
                // 删除空列表
                if (listTypeLength(o) == 0) dbDelete(rl->db,rl->key);
                /* We don't call signalModifiedKey() as it was already called
                 * when an element was pushed on the list. */
            }

            /* Free this item. */
            decrRefCount(rl->key);
            zfree(rl);
            listDelNode(l,ln);
        }
        listRelease(l); /* We have the new list on place at this point. */
    }
}

int getTimeoutFromObjectOrReply(redisClient *c, robj *object, time_t *timeout) {
    long tval;

    if (getLongFromObjectOrReply(c,object,&tval,
        "timeout is not an integer or out of range") != REDIS_OK)
        return REDIS_ERR;

    if (tval < 0) {
        addReplyError(c,"timeout is negative");
        return REDIS_ERR;
    }

    if (tval > 0) tval += server.unixtime;
    *timeout = tval;

    return REDIS_OK;
}

/* Blocking RPOP/LPOP */
void blockingPopGenericCommand(redisClient *c, int where) {
    robj *o;
    time_t timeout;
    int j;

    // 获取 timeout 参数
    if (getTimeoutFromObjectOrReply(c,c->argv[c->argc-1],&timeout) != REDIS_OK)
        return;

    // 遍历所有 key 
    // 如果找到第一个不为空的列表对象，那么对它进行 POP ，然后返回
    for (j = 1; j < c->argc-1; j++) {
        
        o = lookupKeyWrite(c->db,c->argv[j]);

        // 对象不为空？
        if (o != NULL) {
            // 类型检查
            if (o->type != REDIS_LIST) {
                addReply(c,shared.wrongtypeerr);
                return;
            } else {
                // key 为非空列表？
                if (listTypeLength(o) != 0) {
                    /* Non empty list, this is like a non normal [LR]POP. */
                    // 非空列表，执行普通的 [LR]POP
                    robj *value = listTypePop(o,where);
                    redisAssert(value != NULL);

                    addReplyMultiBulkLen(c,2);
                    addReplyBulk(c,c->argv[j]); // key
                    addReplyBulk(c,value);      // value

                    decrRefCount(value);

                    // 删除空列表
                    if (listTypeLength(o) == 0) dbDelete(c->db,c->argv[j]);

                    signalModifiedKey(c->db,c->argv[j]);
                    server.dirty++;

                    /* Replicate it as an [LR]POP instead of B[LR]POP. */
                    rewriteClientCommandVector(c,2,
                        (where == REDIS_HEAD) ? shared.lpop : shared.rpop,
                        c->argv[j]);
                    return;
                }
            }
        }
    }

    /* If we are inside a MULTI/EXEC and the list is empty the only thing
     * we can do is treating it as a timeout (even with timeout 0). */
    if (c->flags & REDIS_MULTI) {
        addReply(c,shared.nullmultibulk);
        return;
    }

    /* If the list is empty or the key does not exists we must block */
    // 所有给定 key 都为空，进行 block
    blockForKeys(c, c->argv + 1, c->argc - 2, timeout, NULL);
}

void blpopCommand(redisClient *c) {
    blockingPopGenericCommand(c,REDIS_HEAD);
}

void brpopCommand(redisClient *c) {
    blockingPopGenericCommand(c,REDIS_TAIL);
}

void brpoplpushCommand(redisClient *c) {
    time_t timeout;

    // 获取 timeout 参数
    if (getTimeoutFromObjectOrReply(c,c->argv[3],&timeout) != REDIS_OK)
        return;

    // 查找 key 对象
    robj *key = lookupKeyWrite(c->db, c->argv[1]);

    // 不存在？
    if (key == NULL) {
        if (c->flags & REDIS_MULTI) {
            /* Blocking against an empty list in a multi state
             * returns immediately. */
            addReply(c, shared.nullbulk);
        } else {
            /* The list is empty and the client blocks. */
            // 直接等待元素 push 到 key
            blockForKeys(c, c->argv + 1, 1, timeout, c->argv[2]);
        }
    } else {
        if (key->type != REDIS_LIST) {
            addReply(c, shared.wrongtypeerr);
        } else {
            /* The list exists and has elements, so
             * the regular rpoplpushCommand is executed. */
            redisAssertWithInfo(c,key,listTypeLength(key) > 0);
            rpoplpushCommand(c);
        }
    }
}
