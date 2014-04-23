/* htmref.c
 *
 * Copyright (C) 2014 MongoDB, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif

#include <assert.h>
#include <immintrin.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>


#if !defined(DISABLE_ASSERT)
# define ASSERT(a) assert((a))
#else
# define ASSERT(a)
#endif


#define USEC_PER_SEC 1000000U


typedef struct _HtmrefPerfTest HtmrefPerfTest;


typedef void (*HtmrefPerfTestFunc) (HtmrefPerfTest *worker);


struct _HtmrefPerfTest
{
   pthread_mutex_t      mutex;
   pthread_cond_t       cond;
   HtmrefPerfTestFunc   worker_func;
   unsigned             threads_active;
   unsigned             n_threads;
   unsigned             n_iterations;
   int64_t              begin_at;
   int64_t              end_at;
   struct {
      volatile int64_t value;
      int64_t padding[7];
   } counter __attribute__((aligned (64)));
   pthread_spinlock_t   spin;
};


static int64_t
Clock_GetMonotonic (void)
{
   struct timespec ts;

   clock_gettime (CLOCK_MONOTONIC, &ts);
   return (ts.tv_sec * USEC_PER_SEC) + (ts.tv_nsec / 1000U);
}


/*
 *--------------------------------------------------------------------------
 *
 * HtmrefPerfTest_Init --
 *
 *       Initialize the performance test, including mutexes, conditions,
 *       and prepare counter zones.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       worker is initialized.
 *
 *--------------------------------------------------------------------------
 */

static void
HtmrefPerfTest_Init (HtmrefPerfTest *test,           /* OUT */
                     HtmrefPerfTestFunc worker_func, /* IN */
                     int n_threads,                  /* IN */
                     int n_iterations)               /* IN */
{
   ASSERT (test);
   ASSERT (worker_func);
   ASSERT (n_threads > 0);
   ASSERT (n_iterations > 0);

   memset (test, 0, sizeof *test);

   pthread_spin_init (&test->spin, PTHREAD_PROCESS_PRIVATE);
   pthread_mutex_init (&test->mutex, NULL);
   pthread_cond_init (&test->cond, NULL);
   test->worker_func = worker_func;
   test->n_threads = n_threads;
   test->n_iterations = n_iterations;
}


static void
HtmrefPerfTest_Spinlock (HtmrefPerfTest *worker) /* IN */
{
   unsigned i;

   ASSERT (worker);

   for (i = 0; i < worker->n_iterations; i++) {
      pthread_spin_lock (&worker->spin);
      worker->counter.value++;
      pthread_spin_unlock (&worker->spin);
   }
}


/*
 *--------------------------------------------------------------------------
 *
 * HtmrefPerfTest_Atomic --
 *
 *       Atomically increment the counter using __sync_fetch_and_add.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

static void
HtmrefPerfTest_Atomic (HtmrefPerfTest *worker) /* IN */
{
   unsigned i;

   ASSERT (worker);

   for (i = 0; i < worker->n_iterations; i++) {
      __sync_fetch_and_add (&worker->counter.value, 1);
   }
}


/*
 *--------------------------------------------------------------------------
 *
 * HtmrefPerfTest_Addq --
 *
 *       Beware of this function. An optimizer could unroll all of the
 *       loops. If the result looks incredible, it probably is.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

static void
HtmrefPerfTest_Addq (HtmrefPerfTest *worker) /* IN */
{
   unsigned i;

   ASSERT (worker);

   for (i = 0; i < worker->n_iterations; i++) {
      worker->counter.value++;
   }
}


/*
 *--------------------------------------------------------------------------
 *
 * HtmrefPerfTest_IntelTsx --
 *
 *       Perform an increment of the reference count using Intel TSX
 *       extensions.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

static void
HtmrefPerfTest_IntelTsx (HtmrefPerfTest *worker) /* IN */
{
   unsigned i;
   int status;

   ASSERT (worker);

   for (i = 0; i < worker->n_iterations; i++) {
      if ((status = _xbegin ()) == _XBEGIN_STARTED) {
         worker->counter.value++;
         _xend ();
      } else {
         __sync_fetch_and_add (&worker->counter.value, 1);
      }
   }
}


/*
 *--------------------------------------------------------------------------
 *
 * HtmrefPerfTest_ThreadStart --
 *
 *       Thread start function that will wait for all peer threads to
 *       start and then begin execution upon notification from the primary
 *       thread.
 *
 *       We wait for all threads to start to ensure we are properly racing
 *       as this test is meant to exploit.
 *
 * Returns:
 *       NULL always.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

static void *
HtmrefPerfTest_ThreadStart (void *data) /* IN */
{
   HtmrefPerfTest *worker = (HtmrefPerfTest *)data;

   pthread_mutex_lock (&worker->mutex);
   worker->threads_active++;
   pthread_cond_wait (&worker->cond, &worker->mutex);
   pthread_mutex_unlock (&worker->mutex);

   worker->worker_func (worker);

   return NULL;
}


/*
 *--------------------------------------------------------------------------
 *
 * HtmrefPerfTest_Run --
 *
 *       Run the performance test using the counter implementation passed
 *       to HtmrefPerfTest_Init().
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

static void
HtmrefPerfTest_Run (HtmrefPerfTest *test)
{
   pthread_attr_t attr;
   pthread_t *threads;
   unsigned i;
   void *ret;
   int again;

   /*
    * Allocate thread state so we can join them later.
    */
   threads = (pthread_t *)calloc (test->n_threads, sizeof (pthread_t));
   if (!threads) {
      fprintf (stderr, "Failed to allocate pthread_t state.\n");
      abort ();
   }

   /*
    * 256-kb stack size should be plenty for this.
    */
   pthread_attr_init (&attr);
   pthread_attr_setstacksize (&attr, 1024 * 1024 * 256);

   /*
    * Spawn n_threads for concurrent counter increment tests.
    */
   for (i = 0; i < test->n_threads; i++) {
      pthread_create (&threads [i], &attr, HtmrefPerfTest_ThreadStart, test);
   }

   /*
    * Wait until all threads have started and are blocked on pthread_cond_wait()
    * so that we can wake them all up simultaneously.
    */
   do {
      pthread_mutex_lock (&test->mutex);
      again = (test->threads_active != test->n_threads);
      pthread_mutex_unlock (&test->mutex);
   } while (again);

   test->begin_at = Clock_GetMonotonic ();

   /*
    * Wake up all of the threads to perform counter tests.
    */
   pthread_mutex_lock (&test->mutex);
   pthread_cond_broadcast (&test->cond);
   pthread_mutex_unlock (&test->mutex);

   /*
    * Block while we join all of the active threds.
    */
   for (i = 0; i < test->n_threads; i++) {
      pthread_join (threads [i], &ret);
   }

   test->end_at = Clock_GetMonotonic ();

   free (threads);
}


static void
HtmrefPerfTest_Destroy (HtmrefPerfTest *test) /* IN */
{
   ASSERT (test);

   pthread_mutex_destroy (&test->mutex);
   pthread_cond_destroy (&test->cond);
}


/*
 *--------------------------------------------------------------------------
 *
 * HtmrefPerfTest_PrintStats --
 *
 *       Print the runtime statistics for the performance test.
 *
 *       if @use_csv is non-zero, the output will be in csv.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

static void
HtmrefPerfTest_PrintStats (HtmrefPerfTest *test, /* IN */
                           const char *command,  /* IN */
                           int run_number)       /* IN */
{
   int64_t expected;
   int64_t time_diff;
   int64_t sec;
   int64_t usec;
   double per_second;

   expected = (int64_t)test->n_threads * (int64_t)test->n_iterations;

   time_diff = test->end_at - test->begin_at;
   sec = time_diff / USEC_PER_SEC;
   usec = time_diff % USEC_PER_SEC;

   per_second = (double)expected / (double)time_diff * 1000000.0;

#if 0
   printf ("command n_threads n_iterations expected actual per_second time\n");
#endif
   printf ("%s %u %u %"PRId64" %"PRId64" %lf %u.%06u\n",
           command, test->n_threads, test->n_iterations,
           expected, test->counter.value, per_second,
           (unsigned)sec, (unsigned)usec);
}


/*
 *--------------------------------------------------------------------------
 *
 * usage --
 *
 *       Print application usage to the stream @f.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

static void
usage (FILE *f,             /* IN */
       const char *prgname) /* IN */
{
   fprintf (f, "\n\
usage: %s COMMAND [N_THREADS [N_ITERATIONS N_RUNS]]]\n\
\n\
  This program tests various implementations of reference counting.\n\
  Not all implementations are guaranteed full correctness.\n\
\n\
\n\
Commands:\n\
  tsx           Use Intel TSX instructions for Transactional Memory.\n\
  addq          Use ++ for reference count incrementing.\n\
  atomic        Use atomic intrinsics for thread-safe reference counts.\n\
  spinlock      Use pthread_spinlock_t to guard counter.\n\
\n\
\n\
Examples:\n\
  %s tsx 10 100000\n\
  %s atomic\n\
  %s addq 1 1000000 3\n\
  %s spinlock\n\
\n", prgname, prgname, prgname, prgname, prgname);
}


/*
 *--------------------------------------------------------------------------
 *
 * main --
 *
 *       Application entry point.
 *
 *       While parsing command line arguments we find the implementation
 *       of counter increments we are to use. Initialize the test case
 *       to use that function and then fire off the desired number of
 *       threads to perform the test.
 *
 *       We do our best to ensure that all threads start at as close of
 *       time as allowed by the scheduler and number of CPUs.
 *
 * Returns:
 *       EXIT_FAILURE on failure, EXIT_SUCCESS on success.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

int
main (int argc,    /* IN */
      char **argv) /* IN */
{
   HtmrefPerfTestFunc worker_func = NULL;
   HtmrefPerfTest test;
   int n_iterations = 10000000;
   int n_threads = 10;
   int n_runs = 1;
   int i;

   if (argc < 2) {
      usage (stderr, argv [0]);
      return EXIT_FAILURE;
   }

   if (0 == strcmp (argv [1], "tsx")) {
      worker_func = HtmrefPerfTest_IntelTsx;
   } else if (0 == strcmp (argv [1], "atomic")) {
      worker_func = HtmrefPerfTest_Atomic;
   } else if (0 == strcmp (argv [1], "addq")) {
      worker_func = HtmrefPerfTest_Addq;
   } else if (0 == strcmp (argv [1], "spinlock")) {
      worker_func = HtmrefPerfTest_Spinlock;
   } else {
      fprintf (stderr, "No such command: %s\n\n", argv [1]);
      usage (stderr, argv [0]);
      return EXIT_FAILURE;
   }

   if (argc > 2) {
      n_threads = atoi (argv [2]);
      if ((n_threads <= 0) || (n_threads > 10000)) {
         fprintf (stderr, "Please specify a resonable thread count.\n");
         usage (stderr, argv [0]);
         return EXIT_FAILURE;
      }
   }

   if (argc > 3) {
      n_iterations = atoi (argv [3]);
      if (n_iterations <= 0) {
         fprintf (stderr, "Please specify a resonable iteration count.\n");
         usage (stderr, argv [0]);
         return EXIT_FAILURE;
      }
   }

   if (argc > 4) {
      n_runs = atoi (argv [4]);
      if (n_runs <= 0) {
         fprintf (stderr, "Please specify a reasonable number of runs.\n");
         usage (stderr, argv [0]);
         return EXIT_FAILURE;
      }
   }

   for (i = 0; i < n_runs; i++) {
      HtmrefPerfTest_Init (&test, worker_func, n_threads, n_iterations);
      HtmrefPerfTest_Run (&test);
      HtmrefPerfTest_PrintStats (&test, argv [1], i);
      HtmrefPerfTest_Destroy (&test);
   }

   return EXIT_SUCCESS;
}

