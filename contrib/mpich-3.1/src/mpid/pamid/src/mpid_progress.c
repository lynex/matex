/* begin_generated_IBM_copyright_prolog                             */
/*                                                                  */
/* This is an automatically generated copyright prolog.             */
/* After initializing,  DO NOT MODIFY OR MOVE                       */
/*  --------------------------------------------------------------- */
/* Licensed Materials - Property of IBM                             */
/* Blue Gene/Q 5765-PER 5765-PRP                                    */
/*                                                                  */
/* (C) Copyright IBM Corp. 2011, 2012 All Rights Reserved           */
/* US Government Users Restricted Rights -                          */
/* Use, duplication, or disclosure restricted                       */
/* by GSA ADP Schedule Contract with IBM Corp.                      */
/*                                                                  */
/*  --------------------------------------------------------------- */
/*                                                                  */
/* end_generated_IBM_copyright_prolog                               */
/*  (C)Copyright IBM Corp.  2007, 2011  */
/**
 * \file src/mpid_progress.c
 * \brief Maintain the state and rules of the MPI progress semantics
 */
#include <mpidimpl.h>


void
MPIDI_Progress_init()
{
  MPIDI_Process.async_progress.active = 0;
  pamix_progress_t async_progress_type=0;

  /* In the "global" mpich lock mode the only possible progress function
   * is the "global mpich lock" trigger progress function. This progress
   * function is ignored if async progress is disabled.
   */
  pamix_progress_function progress_fn = MPIDI_Progress_async_poll;

#if (MPIU_THREAD_GRANULARITY == MPIU_THREAD_GRANULARITY_PER_OBJECT)
  /* In the "per object" mpich lock mode the only possible progress functions
   * are the "context lock" trigger progress function and the 'NULL' progress
   * function.
   */
  if (MPIDI_Process.async_progress.mode == ASYNC_PROGRESS_MODE_LOCKED)
    progress_fn = NULL;
  else
    progress_fn = MPIDI_Progress_async_poll_perobj;

  MPIDI_Process.perobj.context_post.active =
    MPIDI_Process.perobj.context_post.requested;
#endif

  async_progress_type = PAMIX_PROGRESS_ALL;

#ifdef __PE__
  if (MPIDI_Process.mp_interrupts)
  {
      /*  PAMIX_PROGRESS_TIMER, PAMIX_PROGRESS_RECV_INTERRUPT or      */
      /*  ASYNC_PROGRESS_ALL                                          */
      /*  mp_interrupts=0 indicates the interrupts is disabled        */
      /*  which is the same definition as that of PAMIX_PROGRESS_ALL. */
      /*  ASYNC_PROGRESS_ALL is an internal variable which is defined */
      /*  as 0x1111 and needs to be converted to corresponding pami   */
      /*  type PAMIX_PROGRESS_ALL                                     */
      async_progress_type = MPIDI_Process.mp_interrupts;
      if (MPIDI_Process.mp_interrupts == ASYNC_PROGRESS_ALL)
         async_progress_type = PAMIX_PROGRESS_ALL;
  }
#endif


  if (MPIDI_Process.async_progress.mode != ASYNC_PROGRESS_MODE_DISABLED)
    {
      TRACE_ERR("Async advance beginning...\n");

      /* Enable async progress on all contexts.*/
      uintptr_t i;
      for (i=0; i<MPIDI_Process.avail_contexts; ++i)
        {
          PAMIX_Progress_register(MPIDI_Context[i],
                                  progress_fn,
                                  MPIDI_Progress_async_end,
                                  MPIDI_Progress_async_start,
                                  (void *) i);
          PAMIX_Progress_enable(MPIDI_Context[i], async_progress_type);
        }
      TRACE_ERR("Async advance enabled\n");
    }
}

void
MPIDI_Progress_async_start(pami_context_t context, void *cookie)
{
  /* This special async progress critical section is required for both 'global'
   * and 'perobj' mpich lock modes because, regardless of the lock mode, if
   * more than one context is enabled for async progress then multiple threads
   * could attempt to update the shared variables at the same time.
   */
  MPIU_THREAD_CS_ENTER(ASYNC,);

#if (MPIU_THREAD_GRANULARITY == MPIU_THREAD_GRANULARITY_PER_OBJECT)
  if (MPIDI_Process.async_progress.active == 0)
    {
      /* Asynchronous progress was completely disabled and now async progress
       * is enabled for one context. Because one context is in async progress
       * mode the configuration state must be updated as if all contexts were in
       * async rogress mode.
       */
      MPIDI_Process.perobj.context_post.active =
        MPIDI_Process.perobj.context_post.requested;
    }
#endif

  MPIDI_Process.async_progress.active += 1;

  MPIDI_Mutex_sync();

  /* Required for both 'global' and 'perobj' mpich lock modes. */
  MPIU_THREAD_CS_EXIT(ASYNC,);
}

void
MPIDI_Progress_async_end  (pami_context_t context, void *cookie)
{
  /* This special async progress critical section is required for both 'global'
   * and 'perobj' mpich lock modes because, regardless of the lock mode, if
   * more than one context is enabled for async progress then multiple threads
   * could attempt to update the shared variables at the same time.
   */
  MPIU_THREAD_CS_ENTER(ASYNC,);

  MPIDI_Process.async_progress.active -= 1;

#if (MPIU_THREAD_GRANULARITY == MPIU_THREAD_GRANULARITY_PER_OBJECT)
  if (MPIDI_Process.async_progress.active == 0)
    {
      /* Asynchronous progress is now completely disabled on all contexts. */
      if (MPIR_ThreadInfo.thread_provided != MPI_THREAD_MULTIPLE)
        {
          /* The runtime is, currently, completely single threaded - there are
           * no async progress threads and only one application thread. In this
           * environment the context post is simply overhead and can be safely
           * avoided.
           */
          MPIDI_Process.perobj.context_post.active = 0;
        }
    }
#endif
  MPIDI_Mutex_sync();

  /* Required for both 'global' and 'perobj' mpich lock modes. */
  MPIU_THREAD_CS_EXIT(ASYNC,);
}

void
MPIDI_Progress_async_poll (pami_context_t context, void *cookie)
{
  pami_result_t rc;
  int loop_count=100;

  /* In the "global" mpich lock mode all application threads must acquire the
   * ALLFUNC global lock upon entry to the API. The async progress thread
   * invoking this progress function also must first acquire the ALLFUNC
   * global lock as it enters the API and before invoking advance as mpich
   * callbacks invoked during advance will not take any additional locks.
   */
  if (MPIU_THREAD_CS_TRY(ALLFUNC,))           /* (0==try_acquire(0)) */
    {
      /* There is a simplifying assertion when in the 'global' mpich lock mode
       * that only a single context is supported. See the discussion in
       * mpich/src/mpid/pamid/src/mpid_init.c for more information.
       */
      rc = PAMI_Context_advancev (MPIDI_Context, 1, loop_count);
      MPID_assert( (rc == PAMI_SUCCESS) || (rc == PAMI_EAGAIN) );

      MPIDI_Mutex_sync();                     /* is this needed ? */
      MPIU_THREAD_CS_EXIT(ALLFUNC,);          /* release(0) */
#ifdef __PE__
      if (rc == PAMI_EAGAIN)
        sched_yield();
#endif
    }

  return;
}

void
MPIDI_Progress_async_poll_perobj (pami_context_t context, void *cookie)
{
  pami_result_t rc;
  int loop_count=100;

  /* In the "per object" mpich lock mode multiple application threads could be
   * active within the API interacting with contexts and multiple async progress
   * threads could be polling contexts in this function. The only way to force a
   * serialized access to the contexts is to use the pami context locks.
   */
  rc = PAMI_Context_trylock_advancev(MPIDI_Context, MPIDI_Process.avail_contexts, loop_count);
  MPID_assert( (rc == PAMI_SUCCESS) || (rc == PAMI_EAGAIN) );
#ifdef __PE__
  if (rc == PAMI_EAGAIN)
    {
      MPIDI_Mutex_sync();                     /* is this needed ? */
      sched_yield();
    }
#endif

  return;
}

