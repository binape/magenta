#define _GNU_SOURCE

#include "futex_impl.h"
#include "pthread_impl.h"

#include <magenta/syscalls.h>
#include <pthread.h>
#include <runtime/tls.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>

#define ROUND(x) (((x) + PAGE_SIZE - 1) & -PAGE_SIZE)

/* pthread_key_create.c overrides this */
static volatile size_t dummy = 0;
weak_alias(dummy, __pthread_tsd_size);

static void dummy_0(void) {}
weak_alias(dummy_0, __acquire_ptc);
weak_alias(dummy_0, __dl_thread_cleanup);
weak_alias(dummy_0, __do_orphaned_stdio_locks);
weak_alias(dummy_0, __pthread_tsd_run_dtors);
weak_alias(dummy_0, __release_ptc);

void* __mmap(void*, size_t, int, int, int, off_t);
int __munmap(void*, size_t);

static int thread_entry(void* arg) {
    struct __mx_thread_info* ei = arg;
    if (ei->tls) {
        mxr_tls_root_set(ei->tls);
        mxr_tls_set(MXR_TLS_SLOT_ERRNO, &ei->errno_value);
    }
    ei->func(ei->arg);
    mx_thread_exit();
    return 0;
}

int pthread_create(pthread_t* restrict res, const pthread_attr_t* restrict attrp,
                   void* (*entry)(void*), void* restrict arg) {
    pthread_attr_t attr = {0};
    if (attrp)
        attr = *attrp;

    mx_handle_t handle;

    mx_tls_root_t* self_tls = mxr_tls_root_get();
    size_t len;
    mx_proc_info_t* proc = NULL;
    if (self_tls) {
        proc = self_tls->proc;
        len = ROUND(sizeof(struct pthread) + sizeof(mx_tls_root_t) + (sizeof(void*) * (MX_TLS_MIN_SLOTS - 1)) + __pthread_tsd_size);
    } else {
        len = ROUND(sizeof(struct pthread));
    }

    void* map = __mmap(NULL, len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
    if (map == MAP_FAILED)
        return ERR_NO_MEMORY;
    pthread_t thread = map;
    thread->map_base = map;
    thread->mx_thread_info.func = entry;
    thread->mx_thread_info.arg = arg;
    thread->map_size = len;

    if (self_tls) {
        mx_tls_root_t* tls = map + sizeof(*thread);
        tls->magic = MX_TLS_ROOT_MAGIC;
        tls->flags = 0;
        tls->maxslots = MX_TLS_MIN_SLOTS;
        tls->proc = proc;
        tls->self = tls;

        tls->slots[__pthread_key] = thread;
        thread->mx_thread_info.tls = tls;
        thread->tsd = map + len - __pthread_tsd_size;
    }

    const char* name = attr.__name ? attr.__name : "";

    // XXX reimplement in terms of mxr_thread or new thread creation syscalls
#if 0
    handle = mx_thread_create(thread_entry, &thread->mx_thread_info,
                              name, strlen(name));
#else
    (void)name;
    handle = -1;
#endif
    if (handle < 0) {
        __munmap(map, len);
        return handle;
    } else {
        thread->self = thread;
        thread->handle = handle;
        *res = thread;
        return 0;
    }
}

_Noreturn void pthread_exit(void* result) {
    pthread_t self = __pthread_self();
    sigset_t set;

    self->canceldisable = 1;
    self->cancelasync = 0;
    self->result = result;

    while (self->cancelbuf) {
        void (*f)(void*) = self->cancelbuf->__f;
        void* x = self->cancelbuf->__x;
        self->cancelbuf = self->cancelbuf->__next;
        f(x);
    }

    __pthread_tsd_run_dtors();

    mtx_lock(&self->exitlock);

    /* Mark this thread dead before decrementing count */
    mtx_lock(&self->killlock);
    self->dead = 1;

    /* Block all signals before decrementing the live thread count.
     * This is important to ensure that dynamically allocated TLS
     * is not under-allocated/over-committed, and possibly for other
     * reasons as well. */
    __block_all_sigs(&set);

    /* Wait to unlock the kill lock, which governs functions like
     * pthread_kill which target a thread id, until signals have
     * been blocked. This precludes observation of the thread id
     * as a live thread (with application code running in it) after
     * the thread was reported dead by ESRCH being returned. */
    mtx_unlock(&self->killlock);

    /* TODO(kulakowski) Thread exit process teardown. */
    /* It's impossible to determine whether this is "the last thread"
     * until performing the atomic decrement, since multiple threads
     * could exit at the same time. For the last thread, revert the
     * decrement and unblock signals to give the atexit handlers and
     * stdio cleanup code a consistent state. */
    /* if (a_fetch_add(&libc.threads_minus_1, -1) == 0) { */
    /*     libc.threads_minus_1 = 0; */
    /*     __restore_sigs(&set); */
    /*     exit(0); */
    /* } */

    /* TODO(kulakowski): Pthread robust mutex processing used to occur
     * inside this vm lock/unlock pair. I don't if there is also
     * implicitly a need to synchronize on this lock in this function
     * in any case, so I'm leaving the lock/unlock pair.
     */
    __vm_lock();
    __vm_unlock();

    __do_orphaned_stdio_locks();
    __dl_thread_cleanup();

    if (self->detached && self->map_base) {
        /* Detached threads must avoid the kernel clear_child_tid
         * feature, since the virtual address will have been
         * unmapped and possibly already reused by a new mapping
         * at the time the kernel would perform the write. In
         * the case of threads that started out detached, the
         * initial clone flags are correct, but if the thread was
         * detached later (== 2), we need to clear it here. */
        if (self->detached == 2)
            __syscall(SYS_set_tid_address, 0);

        /* Since __unmapself bypasses the normal munmap code path,
         * explicitly wait for vmlock holders first. */
        __vm_wait();

        /* The following call unmaps the thread's stack mapping
         * and then exits without touching the stack. */
        __unmapself(self->map_base, self->map_size);
    }

    for (;;)
        exit(0);
}

void __do_cleanup_push(struct __ptcb* cb) {
    struct pthread* self = __pthread_self();
    cb->__next = self->cancelbuf;
    self->cancelbuf = cb;
}

void __do_cleanup_pop(struct __ptcb* cb) {
    __pthread_self()->cancelbuf = cb->__next;
}

static int start_c11(void* p) {
    pthread_t self = p;
    int (*start)(void*) = (int (*)(void*))self->start;
    pthread_exit((void*)(uintptr_t)start(self->start_arg));
    return 0;
}

/* pthread_key_create.c overrides this */
static void* dummy_tsd[1] = {0};
weak_alias(dummy_tsd, __pthread_tsd_main);

static FILE* volatile dummy_file = 0;
weak_alias(dummy_file, __stdin_used);
weak_alias(dummy_file, __stdout_used);
weak_alias(dummy_file, __stderr_used);
