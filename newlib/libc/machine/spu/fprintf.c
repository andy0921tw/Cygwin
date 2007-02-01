/*
(C) Copyright IBM Corp. 2006

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.
    * Neither the name of IBM nor the names of its contributors may be
used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

Author: Joel Schopp <jschopp@austin.ibm.com>
*/

#include <_ansi.h>
#include <stdio.h>

#include "c99ppe.h"

#ifdef _HAVE_STDC
#include <stdarg.h>
#else
#include <varargs.h>
#endif

typedef struct
{
  int fp;
  unsigned int pad0[ 3 ];
  _CONST char* fmt;
  unsigned int pad1[ 3 ];
  va_list ap;
} c99_fprintf_t;

#ifndef _REENT_ONLY

int
fprintf(FILE * fp, _CONST char *fmt,...)
{
  int* ret;
  c99_fprintf_t args;

  CHECK_INIT(_REENT);

  ret = (int*) &args;

  args.fp = fp->_fp;
  args.fmt = (char*) fmt;

#ifdef _HAVE_STDC
  va_start (args.ap, fmt);
#else
  va_start (args.ap);
#endif

  send_to_ppe(SPE_C99_SIGNALCODE, SPE_C99_VFPRINTF, &args);

  va_end (args.ap);
  return *ret;
}
#endif /* ! _REENT_ONLY */
