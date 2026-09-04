/* pthread.h — PROBE SHIM, not a port.
 *
 * include/misc.h includes <pthread.h> unconditionally, so without this file
 * not one core source gets past its first include and the build gate
 * measures nothing. This header provides the TYPES and PROTOTYPES the core
 * refers to, and no definitions: anything that actually calls into it fails
 * at link time, by design, so the probe can never accidentally "succeed"
 * by running POSIX threads on Windows.
 *
 * The real AmberWinDDX replaces every use of these with SRWLOCK /
 * CONDITION_VARIABLE / CreateThread, and this file is deleted. Original
 * AmberSSH file.
 */
#ifndef AMBERX_PROBE_PTHREAD_H
#define AMBERX_PROBE_PTHREAD_H

typedef struct amberx_pthread_opaque { void* p; } pthread_t;
typedef struct amberx_pthread_mutex   { void* p; } pthread_mutex_t;
typedef struct amberx_pthread_cond    { void* p; } pthread_cond_t;
typedef struct amberx_pthread_attr    { void* p; } pthread_attr_t;
typedef struct amberx_pthread_mattr   { void* p; } pthread_mutexattr_t;
typedef struct amberx_pthread_cattr   { void* p; } pthread_condattr_t;
typedef unsigned pthread_key_t;
typedef int pthread_once_t;

#define PTHREAD_MUTEX_INITIALIZER { 0 }
#define PTHREAD_COND_INITIALIZER  { 0 }
#define PTHREAD_ONCE_INIT 0

int  pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
int  pthread_join(pthread_t, void**);
pthread_t pthread_self(void);
int  pthread_equal(pthread_t, pthread_t);
int  pthread_mutex_init(pthread_mutex_t*, const pthread_mutexattr_t*);
int  pthread_mutex_destroy(pthread_mutex_t*);
int  pthread_mutex_lock(pthread_mutex_t*);
int  pthread_mutex_unlock(pthread_mutex_t*);
int  pthread_cond_init(pthread_cond_t*, const pthread_condattr_t*);
int  pthread_cond_destroy(pthread_cond_t*);
int  pthread_cond_wait(pthread_cond_t*, pthread_mutex_t*);
int  pthread_cond_signal(pthread_cond_t*);
int  pthread_cond_broadcast(pthread_cond_t*);
int  pthread_once(pthread_once_t*, void (*)(void));
int  pthread_key_create(pthread_key_t*, void (*)(void*));
void* pthread_getspecific(pthread_key_t);
int  pthread_setspecific(pthread_key_t, const void*);

#endif
