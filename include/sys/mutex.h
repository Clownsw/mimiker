#ifndef _SYS_MUTEX_H_
#define _SYS_MUTEX_H_

#include <stdbool.h>
#include <sys/mimiker.h>
#include <sys/lockdep.h>

typedef struct thread thread_t;

/*! \file mutex.h */

/*! \brief Basic synchronization primitive.
 *
 * Locking &default mutex* (MTX_SLEEP) goes to sleep when the mutex is acquired
 * by another thread. This kind of mutex must only be used in *thread context*.
 *
 * Locking *spin mutex* (MTX_SPIN) spins while waiting for another thread to
 * release the mutex. Acquiring spin mutex guarantees that interrupts on
 * processor that the thread is running on will be disabled until the thread
 * releases the mutex. This disables preemption on this processor as well.
 *
 * It's safe to use *spin mutex* in *interrupt context*. If you try to use
 * *sleep mutex* within *interrupt context* the kernel is going to panic!
 *
 * \attention It is forbidden to change context while holding spin lock!
 * \todo How to enforce the condition given above?
 *
 * \warning You must never access mutex fields directly outside of its
 * implementation!
 *
 * \note Mutex must always be released by its owner!
 */
typedef struct mtx {
  atomic_intptr_t m_owner; /*!< stores address of the owner */

#if LOCKDEP
  lock_class_mapping_t m_lockmap;
#endif
} mtx_t;

/* Flags stored in lower 3 bits of m_owner. */
#define MTX_SLEEP 0
#define MTX_SPIN 1
#define MTX_NODEBUG 2
#define MTX_CONTESTED 4
#define MTX_FLAGMASK 7

#if LOCKDEP
#define MTX_INITIALIZER(mutexname, type)                                       \
  (mtx_t) {                                                                    \
    .m_owner = (type), .m_lockmap = LOCKDEP_MAPPING_INITIALIZER(mutexname)     \
  }
#else
#define MTX_INITIALIZER(mutexname, type)                                       \
  (mtx_t) {                                                                    \
    .m_owner = (type)                                                          \
  }
#endif

#define MTX_DEFINE(mutexname, type)                                            \
  mtx_t mutexname = MTX_INITIALIZER(mutexname, type)

/*! \brief Initializes mutex.
 *
 * \note Every mutex has to be initialized before it is used. */
void _mtx_init(mtx_t *m, intptr_t flags, const char *name,
               lock_class_key_t *key);

#define mtx_init(lock, flags)                                                  \
  {                                                                            \
    static lock_class_key_t __key;                                             \
    _mtx_init(lock, flags, #lock, &__key);                                     \
  }

/*! \brief Makes mutex unusable for further locking.
 *
 * \todo Not implemented yet. */
#define mtx_destroy(m)

/*! \brief Check if calling thread is the owner of \a m. */
bool mtx_owned(mtx_t *m);

/*! \brief Fetch mutex owner.
 *
 * \note The function is used by some tests. */
static inline thread_t *mtx_owner(mtx_t *m) {
  return (thread_t *)(m->m_owner & ~MTX_FLAGMASK);
}

/*! \brief Locks sleep mutex (with custom \a waitpt) */
void _mtx_lock(mtx_t *m, const void *waitpt) __no_profile;

/*! \brief Locks sleep mutex.
 *
 * If mutex is already owned, then the thread is inserted into turnstile. */
static inline void mtx_lock(mtx_t *m) {
  _mtx_lock(m, __caller(0));
}

/*! \brief Unlocks sleep mutex */
void mtx_unlock(mtx_t *m) __no_profile;

DEFINE_CLEANUP_FUNCTION(mtx_t *, mtx_unlock);

/*! \brief Locks sleep mutex and unlocks it when leaving current scope.
 *
 * You may safely leave the scope by using `break` or `return`.
 *
 * \warning Do not call `noreturn` functions before you leave the scope!
 */
#define SCOPED_MTX_LOCK(mtx_p)                                                 \
  SCOPED_STMT(mtx_t, mtx_lock, CLEANUP_FUNCTION(mtx_unlock), mtx_p)

/*! \brief Enter scope with sleep mutex locked.
 *
 * Unlocks the mutex when leaving the scope.
 *
 * \sa SCOPED_MTX_LOCK
 */
#define WITH_MTX_LOCK(mtx_p)                                                   \
  WITH_STMT(mtx_t, mtx_lock, CLEANUP_FUNCTION(mtx_unlock), mtx_p)

#endif /* !_SYS_MUTEX_H_ */
