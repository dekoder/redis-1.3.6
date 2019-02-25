/* zmalloc - total amount of allocated memory aware version of malloc()
 *
 * Copyright (c) 2009-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "config.h"

#if defined(__sun)
#define PREFIX_SIZE sizeof(long long)
#else
#define PREFIX_SIZE sizeof(size_t)
#endif

#define increment_used_memory(_n) do { \
    if (zmalloc_thread_safe) { \
        pthread_mutex_lock(&used_memory_mutex);  \
        used_memory += _n; \
        pthread_mutex_unlock(&used_memory_mutex); \
    } else { \
        used_memory += _n; \
    } \
} while(0)

#define decrement_used_memory(_n) do { \
    if (zmalloc_thread_safe) { \
        pthread_mutex_lock(&used_memory_mutex);  \
        used_memory -= _n; \
        pthread_mutex_unlock(&used_memory_mutex); \
    } else { \
        used_memory -= _n; \
    } \
} while(0)

static size_t used_memory = 0; /* 使用中的内存字节计数 */
static int zmalloc_thread_safe = 0; // bool值，0/1
/*
用于对 used_memory 的修改和获取操作上锁，上锁条件见 zmalloc_enable_thread_safeness 注释；
malloc等函数是线程安全的，内部自己有锁，不需要额外上锁。
*/
pthread_mutex_t used_memory_mutex = PTHREAD_MUTEX_INITIALIZER;

static void zmalloc_oom(size_t size) {
    fprintf(stderr, "zmalloc: Out of memory trying to allocate %zu bytes\n",
        size);
    fflush(stderr);
    abort();
}

/*
申请的内存的结构：
    非OSX: | size_t | real_ptr |
    OSX: | real_ptr | // OSX可以通过malloc的指针获取到当前申请的内存的大小
指针大小对外部调用无用，只在这个文件里使用。
启发：局部作用量可以自行设置，只要封装得当，外部无感知，内部使用更方便

增加使用的内存量的计数
返回 real_ptr
*/
void *zmalloc(size_t size) {
    void *ptr = malloc(size+PREFIX_SIZE);

    if (!ptr) zmalloc_oom(size);
#ifdef HAVE_MALLOC_SIZE
    increment_used_memory(redis_malloc_size(ptr));
    return ptr;
#else
    *((size_t*)ptr) = size;
    increment_used_memory(size+PREFIX_SIZE);
    return (char*)ptr+PREFIX_SIZE;
#endif
}

/* 修改指针指向数据的大小
如果传入空指针，则直接申请对应大小的内存；// realloc自身语义
否则realloc，这里只是兼容了增加的size的处理；

tips: 记得检查所有malloc调用的返回值，否则OOM的时候返回指针为空
 */
void *zrealloc(void *ptr, size_t size) {
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
#endif
    size_t oldsize;
    void *newptr;

    if (ptr == NULL) return zmalloc(size);
#ifdef HAVE_MALLOC_SIZE
    oldsize = redis_malloc_size(ptr);
    newptr = realloc(ptr,size);
    if (!newptr) zmalloc_oom(size);

    decrement_used_memory(oldsize);
    increment_used_memory(redis_malloc_size(newptr));
    return newptr;
#else
    realptr = (char*)ptr-PREFIX_SIZE;
    oldsize = *((size_t*)realptr);
    newptr = realloc(realptr,size+PREFIX_SIZE);
    if (!newptr) zmalloc_oom(size);

    *((size_t*)newptr) = size;
    decrement_used_memory(oldsize);
    increment_used_memory(size);
    return (char*)newptr+PREFIX_SIZE;
#endif
}

void zfree(void *ptr) {
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
    size_t oldsize;
#endif

    if (ptr == NULL) return;
#ifdef HAVE_MALLOC_SIZE
    decrement_used_memory(redis_malloc_size(ptr));
    free(ptr);
#else
    realptr = (char*)ptr-PREFIX_SIZE;
    oldsize = *((size_t*)realptr);
    decrement_used_memory(oldsize+PREFIX_SIZE);
    free(realptr);
#endif
}

/* 字符串复制进堆里
tips: strlen是不包含\0的，所以要+1，否则要溢出
 */
char *zstrdup(const char *s) {
    size_t l = strlen(s)+1;
    char *p = zmalloc(l);

    memcpy(p,s,l);
    return p;
}

size_t zmalloc_used_memory(void) {
    size_t um;

    if (zmalloc_thread_safe) pthread_mutex_lock(&used_memory_mutex);
    um = used_memory;
    if (zmalloc_thread_safe) pthread_mutex_unlock(&used_memory_mutex);
    return um;
}

/*
  开启条件： server.vm_enabled && server.vm_max_threads != 0
  默认：                4                       0
  即默认不开启
*/
void zmalloc_enable_thread_safeness(void) {
    zmalloc_thread_safe = 1;
}
