/* sync.h: Header file for cygwin synchronization primitives.

   Copyright 1999 Cygnus Solutions.

   Written by Christopher Faylor <cgf@cygnus.com>

This file is part of Cygwin.

This software is a copyrighted work licensed under the terms of the
Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
details. */

/* FIXME: Note that currently this class cannot be allocated via `new' since
   there are issues with malloc and fork. */
class muto
{
  LONG sync;	/* Used to serialize access to this class. */
  LONG visits;	/* Count of number of times a thread has called acquire. */
  LONG waiters;	/* Number of threads waiting for lock. */
  HANDLE bruteforce; /* event handle used to control waiting for lock. */
  DWORD tid;	/* Thread Id of lock owner. */
public:
  void *operator new (size_t, void *p) {return p;}
  void *operator new (size_t) {return ::new muto; }
  void operator delete (void *) {;} /* can't handle allocated mutos
					currently */

  /* This simple constructor is used for cases where no bruteforce
     event handling is required. */
  muto(): sync(0), visits(0), waiters(-1), bruteforce(0), tid(0) {;}
  /* A more complicated constructor. */
  muto(int inh, const char *name);
  ~muto ();
  int acquire (DWORD ms = INFINITE); /* Acquire the lock. */
  int release ();		     /* Release the lock. */

  /* Return true if caller thread owns the lock. */
  int ismine () {return tid == GetCurrentThreadId ();}
};

/* Use a statically allocated buffer as the storage for a muto */
#define new_muto(__inh, __name) \
({ \
  static NO_COPY char __mbuf[sizeof(class muto) + 100] = {0}; \
  muto *m; \
  m = new (__mbuf) muto ((__inh), (__name)); \
  m; \
})
