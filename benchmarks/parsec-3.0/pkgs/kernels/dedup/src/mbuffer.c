//Copyright 2009 Princeton University
//Written by Christian Bienia

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#ifdef ENABLE_PTHREADS
#include <pthread.h>
#endif //ENABLE_PTHREADS

#ifdef ENABLE_DMALLOC
#include <dmalloc.h>
#endif //ENABLE_DMALLOC


#include "mbuffer.h"


// [transmem] turn on tmcondvars
#ifdef ENABLE_TM
#include <tmcondvar.h>
#include <stdint.h>
// [transmem] In dedup, all calls to mbuffer_realloc shrink the buffer.  Thus
// we know that a sloppy realloc, which makes a new region and manually
// copies bytes over, is correct (though slow).  The problem would come if we
// didn't know that we were shrinking... In that case, we could segfault by
// trying to read uninitialized data while populating the newly resized
// buffer.

// NB: this code was taken from
// https://www.student.cs.uwaterloo.ca/~cs350/common/os161-src-html/memcpy_8c-source.html
static void * tm_memcpy(void *dst, const void *src, size_t len)
{
  size_t i;
  /*
   * memcpy does not support overlapping buffers, so always do it
   * forwards. (Don't change this without adjusting memmove.)
   *
   * For speedy copying, optimize the common case where both pointers
   * and the length are word-aligned, and copy word-at-a-time instead
   * of byte-at-a-time. Otherwise, copy by bytes.
   *
   * The alignment logic below should be portable. We rely on
   * the compiler to be reasonably intelligent about optimizing
   * the divides and modulos out. Fortunately, it is.
   */
  if ((uintptr_t)dst % sizeof(long) == 0 &&
      (uintptr_t)src % sizeof(long) == 0 &&
      len % sizeof(long) == 0)
  {
    long *d = dst;
    const long *s = src;
    for (i=0; i<len/sizeof(long); i++) {
      d[i] = s[i];
    }
  }
  else {
    char *d = dst;
    const char *s = src;
    for (i=0; i<len; i++) {
      d[i] = s[i];
    }
  }
  return dst;
}

// now use tm_memcpy to implement realloc
static void *tm_realloc_shrink(void *ptr, size_t size) {
  void *oldp = ptr;
  void* newp = malloc(size);

  if (newp == NULL)
    return NULL;
  if (size > 0)
    tm_memcpy(newp, oldp, size);
  free(oldp);
  return newp;
}



#endif

#ifdef ENABLE_PTHREADS
// [transmem] No locks in TM mode
#ifndef ENABLE_TM
//Use spin locks instead of mutexes (this file only)
#define ENABLE_SPIN_LOCKS

#ifdef ENABLE_SPIN_LOCKS
typedef pthread_spinlock_t pthread_lock_t;
#define PTHREAD_LOCK_INIT(l) pthread_spin_init(l, PTHREAD_PROCESS_PRIVATE)
#define PTHREAD_LOCK_DESTROY(l) pthread_spin_destroy(l)
#define PTHREAD_LOCK(l) pthread_spin_lock(l)
#define PTHREAD_UNLOCK(l) pthread_spin_unlock(l)
#else
typedef pthread_mutex_t pthread_lock_t;
#define PTHREAD_LOCK_INIT(l) pthread_mutex_init(l, NULL)
#define PTHREAD_LOCK_DESTROY(l) pthread_mutex_destroy(l)
#define PTHREAD_LOCK(l) pthread_mutex_lock(l)
#define PTHREAD_UNLOCK(l) pthread_mutex_unlock(l)
#endif //ENABLE_SPIN_LOCKS

//Array with global locks to use. We use many mutexes to achieve some concurrency.
//FIXME: Merge into mbuffer mcb
//FIXME: Update documentation with latest changes
//Unfortunately mutexes cannot be stored inside the memory buffers because of
//concurrent free operations
pthread_lock_t *locks = NULL;

//Number of locks to use. More locks means higher potential concurrency, assuming lock usage is reasonably balanced.
//We use a prime number for that value to get a simple but effective hash function
#define NUMBER_OF_LOCKS 1021

//A very simple hash function to map memory addresses to a lock id for the locks array
static inline int lock_hash(void *p) {
  return (int)(((unsigned long long int)p) % NUMBER_OF_LOCKS);
}
#endif // ENABLE_TM
#endif //ENABLE_PTHREADS



//Initialize memory buffer subsystem
int mbuffer_system_init() {
#ifdef ENABLE_PTHREADS
  // [transmem] no locks to initialize in TM mode
#ifndef ENABLE_TM
  int i;

  assert(locks==NULL);
  locks = malloc(NUMBER_OF_LOCKS * sizeof(pthread_lock_t));
  if(locks==NULL) return -1;
  for(i=0; i<NUMBER_OF_LOCKS; i++) {
    if(PTHREAD_LOCK_INIT(&locks[i]) != 0) {
      int j;
      for(j=0; j<i; j++) {
        PTHREAD_LOCK_DESTROY(&locks[i]);
      }
      free((void *)locks);
      locks=NULL;
      return -1;
    }
  }
#endif
#endif
  return 0;
}

//Shutdown memory buffer subsystem
int mbuffer_system_destroy() {
#ifdef ENABLE_PTHREADS
  // [transmem] No locks to destroy in TM code
#ifdef ENABLE_TM
  return 0;
#else
  int i, rv;
  rv=0;
  for(i=0; i<NUMBER_OF_LOCKS; i++) {
    rv+=PTHREAD_LOCK_DESTROY(&locks[i]);
  }
  free((void *)locks);
  locks=NULL;
  return rv ? -1 : 0;
#endif
#else
  return 0;
#endif
}

//Initialize a memory buffer
int mbuffer_create(mbuffer_t *m, size_t size) {
  void *ptr;

  // [transmem] skip asserts in TM mode
#ifndef ENABLE_TM
  assert(m!=NULL);
  assert(size > 0);
#endif

  //FIXME: Merge both mallocs to one
  ptr = malloc(size);
  if(ptr==NULL) return -1;
  m->mcb = (mcb_t *)malloc(sizeof(mcb_t));
  if(m->mcb==NULL) {
    free(ptr);
    ptr=NULL;
    return -1;
  }

  m->ptr = ptr;
  m->n = size;
  m->mcb->i = 1;
  m->mcb->ptr = ptr;
#ifdef ENABLE_MBUFFER_CHECK
  m->check_flag=MBUFFER_CHECK_MAGIC;
#endif

  return 0;
}

//Make a shallow copy of a memory buffer
mbuffer_t *mbuffer_clone(mbuffer_t *m) {
  mbuffer_t *temp;

  assert(m!=NULL);
#ifdef ENABLE_MBUFFER_CHECK
  assert(m->check_flag==MBUFFER_CHECK_MAGIC);
#endif

  temp = malloc(sizeof(mbuffer_t));
  if(temp==NULL) return NULL;

  //Update reference counter
#ifdef ENABLE_PTHREADS
  // [transmem] use TM instead of locks
#ifdef ENABLE_TM
  __transaction_atomic { m->mcb->i++; }
#else
  PTHREAD_LOCK(&locks[lock_hash(m->mcb)]);
  assert(m->mcb->i>=1);
  m->mcb->i++;
  PTHREAD_UNLOCK(&locks[lock_hash(m->mcb)]);
#endif
#else
  assert(m->mcb->i>=1);
  m->mcb->i++;
#endif //ENABLE_PTHREADS

  //copy state, use joint mcb
  temp->ptr = m->ptr;
  temp->n = m->n;
  temp->mcb = m->mcb;
#ifdef ENABLE_MBUFFER_CHECK
  temp->check_flag=MBUFFER_CHECK_MAGIC;
#endif

  return temp;
}

//Make a deep copy of a memory buffer
mbuffer_t *mbuffer_copy(mbuffer_t *m) {
  mbuffer_t *temp;

  assert(m!=NULL);
  assert(m->n >= 1);
#ifdef ENABLE_MBUFFER_CHECK
  assert(m->check_flag==MBUFFER_CHECK_MAGIC);
#endif

  //NOTE: No need to update reference counter of master, resulting copy of buffer will be independent
  temp = malloc(sizeof(mbuffer_t));
  if(temp==NULL) return NULL;
  if(mbuffer_create(temp, m->n)!=0) {
    free(temp);
    temp=NULL;
    return NULL;
  }
  memcpy(temp->ptr, m->ptr, m->n);
#ifdef ENABLE_MBUFFER_CHECK
  temp->check_flag=MBUFFER_CHECK_MAGIC;
#endif

  return temp;
}

//Free a memory buffer
void mbuffer_free(mbuffer_t *m) {
  unsigned int ref;

  // [transmem] Disable asserts in TM mode
#ifndef ENABLE_TM
  assert(m!=NULL);
#endif
#ifdef ENABLE_MBUFFER_CHECK
  assert(m->check_flag==MBUFFER_CHECK_MAGIC);
#endif

  //Update meta state first to avoid races
#ifdef ENABLE_PTHREADS
  // [transmem] use TM instead of locks
#ifdef ENABLE_TM
  __transaction_atomic {
    m->mcb->i--;
    ref = m->mcb->i;
  }
#else
  PTHREAD_LOCK(&locks[lock_hash(m->mcb)]);
  m->mcb->i--;
  ref = m->mcb->i;
  PTHREAD_UNLOCK(&locks[lock_hash(m->mcb)]);
#endif
#else
  m->mcb->i--;
  ref = m->mcb->i;
#endif //ENABLE_PTHREADS

  //NOTE: No need to synchronize access to ref counter value again because if it has hit 0 the buffer is dead
  if(ref==0) {
    free(m->mcb->ptr);
    m->mcb->ptr=NULL;
    free(m->mcb);
    m->mcb=NULL;
  }
#ifdef ENABLE_MBUFFER_CHECK
    m->check_flag=0;
#endif
}

//Resize a memory buffer
//Returns 0 if the operation was successful
int mbuffer_realloc(mbuffer_t *m, size_t size) {
  void *r;

  // [transmem] Disable asserts in TM mode
#ifndef ENABLE_TM
  assert(m!=NULL);
  assert(size>0);
#ifdef ENABLE_MBUFFER_CHECK
  assert(m->check_flag==MBUFFER_CHECK_MAGIC);
#endif
#endif

#ifdef ENABLE_PTHREADS
  // [transmem] use TM instead of lock
#ifdef ENABLE_TM
  __transaction_atomic {
#else
  PTHREAD_LOCK(&locks[lock_hash(m->mcb)]);
#endif
#endif //ENABLE_PTHREADS

  //We cannot resize a buffer if more than one pointer to it is in circulation
  if(m->mcb->i > 1) {
#ifdef ENABLE_PTHREADS
    // [transmem] no lock to release...
#ifndef ENABLE_TM
    PTHREAD_UNLOCK(&locks[lock_hash(m->mcb)]);
#endif
#endif
    return -1;
  }
  //This must be the original mbuffer, otherwise we'd have to do something more complicated
  if(m->ptr != m->mcb->ptr) {
#ifdef ENABLE_PTHREADS
    // [transmem] no lock to release...
#ifndef ENABLE_TM
    PTHREAD_UNLOCK(&locks[lock_hash(m->mcb)]);
#endif
#endif
    return -1;
  }

  // [transmem] If we're in TM mode, then since we know it's a shrink, we can
  // realloc using our custom function
#ifdef ENABLE_TM
  r = tm_realloc_shrink(m->ptr, size);
#else
  r = realloc(m->ptr, size);
#endif
  if(r != NULL) {
    m->ptr = r;
    m->n = size;
    m->mcb->ptr = r;
  }

#ifdef ENABLE_PTHREADS
  // [transmem] finish the transaction
#ifdef ENABLE_TM
  }
#else
  PTHREAD_UNLOCK(&locks[lock_hash(m->mcb)]);
#endif
#endif

  if(r == NULL) {
    return -1;
  } else {
    return 0;
  }
}

//Split a memory buffer m1 into two buffers m1 and m2 at the designated location
//Returns 0 if the operation was successful
int mbuffer_split(mbuffer_t *m1, mbuffer_t *m2, size_t split) {
  assert(m1!=NULL);
  assert(m2!=NULL);
  assert(split>0);
  assert(split < m1->n);
#ifdef ENABLE_MBUFFER_CHECK
  assert(m1->check_flag==MBUFFER_CHECK_MAGIC);
  assert(m2->check_flag!=MBUFFER_CHECK_MAGIC);
#endif

  //Update reference counter
#ifdef ENABLE_PTHREADS
  // [transmem] use TM, skip the assert
#ifdef ENABLE_TM
  __transaction_atomic { m1->mcb->i++; }
#else
  PTHREAD_LOCK(&locks[lock_hash(m1->mcb)]);
  assert(m1->mcb->i>=1);
  m1->mcb->i++;
  PTHREAD_UNLOCK(&locks[lock_hash(m1->mcb)]);
#endif
#else
  assert(m1->mcb->i>=1);
  m1->mcb->i++;
#endif //ENABLE_PTHREADS

  //split buffer
  m2->ptr = m1->ptr+split;
  m2->n = m1->n-split;
  m2->mcb = m1->mcb;
  m1->n = split;
#ifdef ENABLE_MBUFFER_CHECK
  m2->check_flag=MBUFFER_CHECK_MAGIC;
#endif

  return 0;
}
