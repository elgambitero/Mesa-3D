/**************************************************************************
 * 
 * Copyright 1999-2006 Brian Paul
 * Copyright 2008 VMware, Inc.
 * All Rights Reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/


/**
 * @file
 * 
 * Thread, mutex, condition variable, barrier, semaphore and
 * thread-specific data functions.
 */


#ifndef OS_THREAD_H_
#define OS_THREAD_H_


#include "pipe/p_compiler.h"
#include "util/u_debug.h" /* for assert */

#include "c11/threads.h"

#ifdef HAVE_PTHREAD
#include <signal.h>
#endif


/* pipe_thread
 */
typedef thrd_t pipe_thread;

#define PIPE_THREAD_ROUTINE( name, param ) \
   int name( void *param )

static inline pipe_thread pipe_thread_create( PIPE_THREAD_ROUTINE((*routine), ), void *param )
{
   pipe_thread thread;
#ifdef HAVE_PTHREAD
   sigset_t saved_set, new_set;
   int ret;

   sigfillset(&new_set);
   pthread_sigmask(SIG_SETMASK, &new_set, &saved_set);
   ret = thrd_create( &thread, routine, param );
   pthread_sigmask(SIG_SETMASK, &saved_set, NULL);
#else
   int ret;
   ret = thrd_create( &thread, routine, param );
#endif
   if (ret)
      return 0;

   return thread;
}

static inline int pipe_thread_wait( pipe_thread thread )
{
   return thrd_join( thread, NULL );
}

static inline int pipe_thread_destroy( pipe_thread thread )
{
   return thrd_detach( thread );
}

static inline void pipe_thread_setname( const char *name )
{
#if defined(HAVE_PTHREAD)
#  if defined(__GNU_LIBRARY__) && defined(__GLIBC__) && defined(__GLIBC_MINOR__) && \
      (__GLIBC__ >= 3 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 12))
   pthread_setname_np(pthread_self(), name);
#  endif
#endif
   (void)name;
}


static inline int pipe_thread_is_self( pipe_thread thread )
{
#if defined(HAVE_PTHREAD)
#  if defined(__GNU_LIBRARY__) && defined(__GLIBC__) && defined(__GLIBC_MINOR__) && \
      (__GLIBC__ >= 3 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 12))
   return pthread_equal(pthread_self(), thread);
#  endif
#endif
   return 0;
}

/* pipe_mutex
 */
typedef mtx_t pipe_mutex;

#define pipe_static_mutex(mutex) \
   static pipe_mutex mutex = _MTX_INITIALIZER_NP

#define pipe_mutex_init(mutex) \
   (void) mtx_init(&(mutex), mtx_plain)

#define pipe_mutex_destroy(mutex) \
   mtx_destroy(&(mutex))

#define pipe_mutex_lock(mutex) \
   (void) mtx_lock(&(mutex))

#define pipe_mutex_unlock(mutex) \
   (void) mtx_unlock(&(mutex))

#define pipe_mutex_assert_locked(mutex) \
   __pipe_mutex_assert_locked(&(mutex))

static inline void
__pipe_mutex_assert_locked(pipe_mutex *mutex)
{
#ifdef DEBUG
   /* NOTE: this would not work for recursive mutexes, but
    * pipe_mutex doesn't support those
    */
   int ret = mtx_trylock(mutex);
   assert(ret == thrd_busy);
   if (ret == thrd_success)
      mtx_unlock(mutex);
#endif
}

/* pipe_condvar
 */
typedef cnd_t pipe_condvar;

#define pipe_condvar_init(cond)	\
   cnd_init(&(cond))

#define pipe_condvar_destroy(cond) \
   cnd_destroy(&(cond))

#define pipe_condvar_wait(cond, mutex) \
   cnd_wait(&(cond), &(mutex))

#define pipe_condvar_signal(cond) \
   cnd_signal(&(cond))

#define pipe_condvar_broadcast(cond) \
   cnd_broadcast(&(cond))


/*
 * pipe_barrier
 */

#if (defined(PIPE_OS_LINUX) || defined(PIPE_OS_BSD) || defined(PIPE_OS_SOLARIS) || defined(PIPE_OS_HURD)) && !defined(PIPE_OS_ANDROID)

typedef pthread_barrier_t pipe_barrier;

static inline void pipe_barrier_init(pipe_barrier *barrier, unsigned count)
{
   pthread_barrier_init(barrier, NULL, count);
}

static inline void pipe_barrier_destroy(pipe_barrier *barrier)
{
   pthread_barrier_destroy(barrier);
}

static inline void pipe_barrier_wait(pipe_barrier *barrier)
{
   pthread_barrier_wait(barrier);
}


#else /* If the OS doesn't have its own, implement barriers using a mutex and a condvar */

typedef struct {
   unsigned count;
   unsigned waiters;
   uint64_t sequence;
   pipe_mutex mutex;
   pipe_condvar condvar;
} pipe_barrier;

static inline void pipe_barrier_init(pipe_barrier *barrier, unsigned count)
{
   barrier->count = count;
   barrier->waiters = 0;
   barrier->sequence = 0;
   pipe_mutex_init(barrier->mutex);
   pipe_condvar_init(barrier->condvar);
}

static inline void pipe_barrier_destroy(pipe_barrier *barrier)
{
   assert(barrier->waiters == 0);
   pipe_mutex_destroy(barrier->mutex);
   pipe_condvar_destroy(barrier->condvar);
}

static inline void pipe_barrier_wait(pipe_barrier *barrier)
{
   pipe_mutex_lock(barrier->mutex);

   assert(barrier->waiters < barrier->count);
   barrier->waiters++;

   if (barrier->waiters < barrier->count) {
      uint64_t sequence = barrier->sequence;

      do {
         pipe_condvar_wait(barrier->condvar, barrier->mutex);
      } while (sequence == barrier->sequence);
   } else {
      barrier->waiters = 0;
      barrier->sequence++;
      pipe_condvar_broadcast(barrier->condvar);
   }

   pipe_mutex_unlock(barrier->mutex);
}


#endif


/*
 * Semaphores
 */

typedef struct
{
   pipe_mutex mutex;
   pipe_condvar cond;
   int counter;
} pipe_semaphore;


static inline void
pipe_semaphore_init(pipe_semaphore *sema, int init_val)
{
   pipe_mutex_init(sema->mutex);
   pipe_condvar_init(sema->cond);
   sema->counter = init_val;
}

static inline void
pipe_semaphore_destroy(pipe_semaphore *sema)
{
   pipe_mutex_destroy(sema->mutex);
   pipe_condvar_destroy(sema->cond);
}

/** Signal/increment semaphore counter */
static inline void
pipe_semaphore_signal(pipe_semaphore *sema)
{
   pipe_mutex_lock(sema->mutex);
   sema->counter++;
   pipe_condvar_signal(sema->cond);
   pipe_mutex_unlock(sema->mutex);
}

/** Wait for semaphore counter to be greater than zero */
static inline void
pipe_semaphore_wait(pipe_semaphore *sema)
{
   pipe_mutex_lock(sema->mutex);
   while (sema->counter <= 0) {
      pipe_condvar_wait(sema->cond, sema->mutex);
   }
   sema->counter--;
   pipe_mutex_unlock(sema->mutex);
}



/*
 * Thread-specific data.
 */

typedef struct {
   tss_t key;
   int initMagic;
} pipe_tsd;


#define PIPE_TSD_INIT_MAGIC 0xff8adc98


static inline void
pipe_tsd_init(pipe_tsd *tsd)
{
   if (tss_create(&tsd->key, NULL/*free*/) != 0) {
      exit(-1);
   }
   tsd->initMagic = PIPE_TSD_INIT_MAGIC;
}

static inline void *
pipe_tsd_get(pipe_tsd *tsd)
{
   if (tsd->initMagic != (int) PIPE_TSD_INIT_MAGIC) {
      pipe_tsd_init(tsd);
   }
   return tss_get(tsd->key);
}

static inline void
pipe_tsd_set(pipe_tsd *tsd, void *value)
{
   if (tsd->initMagic != (int) PIPE_TSD_INIT_MAGIC) {
      pipe_tsd_init(tsd);
   }
   if (tss_set(tsd->key, value) != 0) {
      exit(-1);
   }
}



/*
 * Thread statistics.
 */

/* Return the time of a thread's CPU time clock. */
static inline int64_t
pipe_thread_get_time_nano(pipe_thread thread)
{
#if defined(PIPE_OS_LINUX) && defined(HAVE_PTHREAD)
   struct timespec ts;
   clockid_t cid;

   pthread_getcpuclockid(thread, &cid);
   clock_gettime(cid, &ts);
   return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
#else
   return 0;
#endif
}

/* Return the time of the current thread's CPU time clock. */
static inline int64_t
pipe_current_thread_get_time_nano(void)
{
#if defined(HAVE_PTHREAD)
   return pipe_thread_get_time_nano(pthread_self());
#else
   return 0;
#endif
}

#endif /* OS_THREAD_H_ */
