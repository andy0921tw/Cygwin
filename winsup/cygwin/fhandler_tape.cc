/* fhandler_tape.cc.  See fhandler.h for a description of the fhandler
   classes.

   Copyright 1999, 2000, 2001, 2002, 2003, 2004 Red Hat, Inc.

This file is part of Cygwin.

This software is a copyrighted work licensed under the terms of the
Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
details. */

#include "winsup.h"
#include <sys/termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mtio.h>
#include <ddk/ntddstor.h>
#include "cygerrno.h"
#include "perprocess.h"
#include "security.h"
#include "path.h"
#include "fhandler.h"
#include "dtable.h"
#include "cygheap.h"

/* Media changes and bus resets are sometimes reported and the function
   hasn't been executed.  We repeat all functions which return with one
   of these error codes. */
/* FIXME: Note that this is wrong!  The correct behaviour after getting
   an ERROR_BUS_RESET is to raise a flag and then to block any access,
   except for MTREW, MTOFFL, MT_RETEN, MTERASE, MTSEEK and MTEOM, and
   to set errno to EIO in all other cases. */
#define TAPE_FUNC(func) do { \
			  lasterr = (func); \
			} while (lasterr == ERROR_MEDIA_CHANGED \
				 || lasterr == ERROR_BUS_RESET)

/* Convert LARGE_INTEGER into long long */
#define get_ll(pl)  (((long long) (pl).HighPart << 32) | (pl).LowPart)

#define IS_EOM(err) ((err) == ERROR_END_OF_MEDIA \
		     || (err) == ERROR_EOM_OVERFLOW \
		     || (err) == ERROR_NO_DATA_DETECTED)

#define IS_EOF(err) ((err) == ERROR_FILEMARK_DETECTED \
		     || (err) == ERROR_SETMARK_DETECTED)

/**********************************************************************/
/* fhandler_dev_tape */

void
fhandler_dev_tape::clear (void)
{
  lasterr = 0;
  fhandler_dev_raw::clear ();
}

int
fhandler_dev_tape::is_eom (int win_error)
{
  int ret = IS_EOM (win_error);
  if (ret)
    debug_printf ("end of medium");
  return ret;
}

int
fhandler_dev_tape::is_eof (int win_error)
{
  int ret = IS_EOF (win_error);
  if (ret)
    debug_printf ("end of file");
  return ret;
}

BOOL
fhandler_dev_tape::write_file (const void *buf, DWORD to_write,
			       DWORD *written, int *err)
{
  BOOL ret;

  do
    {
      *err = 0;
      if (!(ret = WriteFile (get_handle (), buf, to_write, written, 0)))
	*err = GetLastError ();
    }
  while (*err == ERROR_MEDIA_CHANGED || *err == ERROR_BUS_RESET);
  syscall_printf ("%d (err %d) = WriteFile (%d, %d, write %d, written %d, 0)",
		  ret, *err, get_handle (), buf, to_write, *written);
  return ret;
}

BOOL
fhandler_dev_tape::read_file (void *buf, DWORD to_read, DWORD *read, int *err)
{
  BOOL ret;

  do
    {
      *err = 0;
      if (!(ret = ReadFile (get_handle (), buf, to_read, read, 0)))
	*err = GetLastError ();
    }
  while (*err == ERROR_MEDIA_CHANGED || *err == ERROR_BUS_RESET);
  syscall_printf ("%d (err %d) = ReadFile (%d, %d, to_read %d, read %d, 0)",
		  ret, *err, get_handle (), buf, to_read, *read);
  return ret;
}

fhandler_dev_tape::fhandler_dev_tape ()
  : fhandler_dev_raw ()
{
  debug_printf ("unit: %d", dev ().minor);
}

int
fhandler_dev_tape::open (int flags, mode_t)
{
  int ret;

  devbufsiz = 1L;

  ret = fhandler_dev_raw::open (flags);
  if (ret)
    {
      DWORD varlen;
      struct mtget get;
      unsigned long block;

      TAPE_FUNC (GetTapeParameters (get_handle (), GET_TAPE_DRIVE_INFORMATION,
				    (varlen = sizeof dp, &varlen), &dp));

      if (!tape_status (&get))
        {
	  long blksize = (get.mt_dsreg & MT_ST_BLKSIZE_MASK)
	  		 >> MT_ST_BLKSIZE_SHIFT;
	  /* Tape drive supports and is set to variable block size. */
	  if (blksize == 0)
	    devbufsiz = get.mt_maxblksize;
	  else
	    devbufsiz = blksize;
	  varblkop = blksize == 0;
	}

      if (devbufsiz > 1L)
	devbuf = new char [devbufsiz];

      /* The following rewind in position 0 solves a problem which appears
       * in case of multi volume archives: The last ReadFile on first medium
       * returns ERROR_NO_DATA_DETECTED. After media change, all subsequent
       * ReadFile calls return ERROR_NO_DATA_DETECTED, too.
       * The call to tape_set_pos seems to reset some internal flags. */
      if (!tape_get_pos (&block) && !block)
	{
	  debug_printf ("rewinding");
	  tape_set_pos (TAPE_REWIND, 0);
	}

      if (flags & O_APPEND)
	{
	  /* In append mode, seek to beginning of next filemark */
	  tape_set_pos (TAPE_SPACE_FILEMARKS, 1, true);
	}
    }

  return ret;
}

int
fhandler_dev_tape::close (void)
{
  struct mtop op;
  int ret = 0;

  if (is_writing)
    {
      ret = writebuf ();
      if (has_written && !eom_detected)
	{
	  /* if last operation was writing, write a filemark */
	  debug_printf ("writing filemark");
	  op.mt_op = MTWEOF;
	  op.mt_count = 1;
	  ioctl (MTIOCTOP, &op);
	}
    }

  // To protected reads on signaling (e.g. Ctrl-C)
  eof_detected = 1;

  if (is_rewind_device ())
    {
      debug_printf ("rewinding");
      tape_set_pos (TAPE_REWIND, 0);
    }

  if (ret)
    {
      fhandler_dev_raw::close ();
      return ret;
    }

  return fhandler_dev_raw::close ();
}

int
fhandler_dev_tape::fstat (struct __stat64 *buf)
{
  int ret;

  if (!(ret = fhandler_base::fstat (buf)))
    {
      struct mtget get;

      if (!ioctl (MTIOCGET, &get))
	buf->st_blocks = get.mt_capacity / buf->st_blksize;
    }

  return ret;
}

_off64_t
fhandler_dev_tape::lseek (_off64_t offset, int whence)
{
  struct mtop op;
  struct mtpos pos;

  debug_printf ("lseek (%s, %d, %d)", get_name (), offset, whence);

  writebuf ();

  if (ioctl (MTIOCPOS, &pos))
    {
      return ILLEGAL_SEEK;
    }

  switch (whence)
    {
      case SEEK_END:
	op.mt_op = MTFSF;
	op.mt_count = 1;
	if (ioctl (MTIOCTOP, &op))
	  return -1;
	break;
      case SEEK_SET:
	if (whence == SEEK_SET && offset < 0)
	  {
	    set_errno (EINVAL);
	    return -1;
	  }
	break;
      case SEEK_CUR:
	break;
      default:
	set_errno (EINVAL);
	return -1;
    }

  op.mt_op = MTFSR;
  op.mt_count = offset / devbufsiz
		- (whence == SEEK_SET ? pos.mt_blkno : 0);

  if (op.mt_count < 0)
    {
      op.mt_op = MTBSR;
      op.mt_count = -op.mt_count;
    }

  if (ioctl (MTIOCTOP, &op) || ioctl (MTIOCPOS, &pos))
    return -1;

  return (pos.mt_blkno * devbufsiz);
}

int
fhandler_dev_tape::dup (fhandler_base *child)
{
  fhandler_dev_tape *fhc = (fhandler_dev_tape *) child;

  fhc->lasterr = lasterr;
  fhc->dp = dp;
  return fhandler_dev_raw::dup (child);
}

int
fhandler_dev_tape::ioctl (unsigned int cmd, void *buf)
{
  int ret = NO_ERROR;
  unsigned long block;

  if (cmd == MTIOCTOP)
    {
      struct mtop *op = (struct mtop *) buf;

      if (!op)
	ret = ERROR_INVALID_PARAMETER;
      else
	switch (op->mt_op)
	  {
	  case MTRESET:
	    break;
	  case MTFSF:
	    ret = tape_set_pos (TAPE_SPACE_FILEMARKS, op->mt_count);
	    break;
	  case MTBSF:
	    ret = tape_set_pos (TAPE_SPACE_FILEMARKS, -op->mt_count);
	    break;
	  case MTFSR:
	    ret = tape_set_pos (TAPE_SPACE_RELATIVE_BLOCKS, op->mt_count);
	    break;
	  case MTBSR:
	    ret = tape_set_pos (TAPE_SPACE_RELATIVE_BLOCKS, -op->mt_count);
	    break;
	  case MTWEOF:
	    if (tape_get_feature (TAPE_DRIVE_WRITE_FILEMARKS))
	      ret = tape_write_marks (TAPE_FILEMARKS, op->mt_count);
	    else if (tape_get_feature (TAPE_DRIVE_WRITE_LONG_FMKS))
	      ret = tape_write_marks (TAPE_LONG_FILEMARKS, op->mt_count);
	    else
	      ret = tape_write_marks (TAPE_SHORT_FILEMARKS, op->mt_count);
	    break;
	  case MTREW:
	    ret = tape_set_pos (TAPE_REWIND, 0);
	    break;
	  case MTOFFL:
	  case MTUNLOAD:
	    ret = tape_prepare (TAPE_UNLOAD);
	    break;
	  case MTNOP:
	    reset_devbuf ();
	    break;
	  case MTRETEN:
	    if (!tape_get_feature (TAPE_DRIVE_END_OF_DATA))
	      ret = ERROR_INVALID_PARAMETER;
	    else if (!(ret = tape_set_pos (TAPE_REWIND, 0, false)))
	      ret = tape_prepare (TAPE_TENSION);
	    break;
	  case MTBSFM:
	    ret = tape_set_pos (TAPE_SPACE_FILEMARKS, -op->mt_count, true);
	    break;
	  case MTFSFM:
	    ret = tape_set_pos (TAPE_SPACE_FILEMARKS, op->mt_count, true);
	    break;
	  case MTEOM:
	    if (tape_get_feature (TAPE_DRIVE_END_OF_DATA))
	      ret = tape_set_pos (TAPE_SPACE_END_OF_DATA, 0);
	    else
	      ret = tape_set_pos (TAPE_SPACE_FILEMARKS, 32767);
	    break;
	  case MTERASE:
	    ret = tape_erase (TAPE_ERASE_LONG);
	    break;
	  case MTRAS1:
	  case MTRAS2:
	  case MTRAS3:
	    ret = ERROR_INVALID_PARAMETER;
	    break;
	  case MTSETBLK:
	    if (!tape_get_feature (TAPE_DRIVE_SET_BLOCK_SIZE))
	      {
		ret = ERROR_INVALID_PARAMETER;
		break;
	      }
	    if ((devbuf && (size_t) op->mt_count == devbufsiz)
		|| (!devbuf && op->mt_count == 0))
	      {
		/* Nothing has changed. */
		ret = 0;
		break;
	      }
	    if ((op->mt_count == 0
		 && !tape_get_feature (TAPE_DRIVE_VARIABLE_BLOCK))
		|| (op->mt_count > 0
		    && ((DWORD) op->mt_count < dp.MinimumBlockSize
			|| (DWORD) op->mt_count > dp.MaximumBlockSize)))
	      {
		ret = ERROR_INVALID_PARAMETER;
		break;
	      }
	    if (devbuf && devbufend - devbufstart > 0
		&& (op->mt_count == 0
		    || (op->mt_count > 0
			&& (size_t) op->mt_count < devbufend - devbufstart)))
	      {
		/* Not allowed if still data in devbuf. */
		ret = ERROR_INVALID_BLOCK_LENGTH; /* EIO */
		break;
	      }
	    if (!(ret = tape_set_blocksize (op->mt_count)))
	      {
		char *buf = NULL;
		if (op->mt_count > 1L && !(buf = new char [op->mt_count]))
		  {
		    ret = ERROR_OUTOFMEMORY;
		    break;
		  }
		if (devbufsiz > 1L && op->mt_count > 1L)
		  {
		    memcpy (buf, devbuf + devbufstart,
			    devbufend - devbufstart);
		    devbufend -= devbufstart;
		  }
		else
		  devbufend = 0;
		devbufstart = 0;
		delete [] devbuf;
		devbuf = buf;
		devbufsiz = op->mt_count;
		varblkop = op->mt_count == 0;
	      }
	    reset_devbuf ();
	    break;
	  case MTSEEK:
	    if (tape_get_feature (TAPE_DRIVE_LOGICAL_BLK))
	      ret = tape_set_pos (TAPE_LOGICAL_BLOCK, op->mt_count);
	    else if (!(ret = tape_get_pos (&block)))
	      ret = tape_set_pos (TAPE_SPACE_RELATIVE_BLOCKS,
				  op->mt_count - block);
	    break;
	  case MTTELL:
	    if (!(ret = tape_get_pos (&block)))
	      op->mt_count = block;
	    break;
	  case MTFSS:
	    ret = tape_set_pos (TAPE_SPACE_SETMARKS, op->mt_count);
	    break;
	  case MTBSS:
	    ret = tape_set_pos (TAPE_SPACE_SETMARKS, -op->mt_count);
	    break;
	  case MTWSM:
	    ret = tape_write_marks (TAPE_SETMARKS, op->mt_count);
	    reset_devbuf ();
	    break;
	  case MTLOCK:
	    ret = tape_prepare (TAPE_LOCK);
	    break;
	  case MTUNLOCK:
	    ret = tape_prepare (TAPE_UNLOCK);
	    break;
	  case MTLOAD:
	    ret = tape_prepare (TAPE_LOAD);
	    break;
	  case MTCOMPRESSION:
	    ret = tape_compression (op->mt_count);
	    break;
	  case MTSETPART:
	    ret = tape_set_partition (op->mt_count);
	    reset_devbuf ();
	    break;
	  case MTMKPART:
	    ret = tape_partition (op->mt_count);
	    reset_devbuf ();
	    break;
	  case MTSETDENSITY:
	  case MTSETDRVBUFFER:
	    reset_devbuf ();
	  default:
	    ret = ERROR_INVALID_PARAMETER;
	    break;
	  }
    }
  else if (cmd == MTIOCGET)
    ret = tape_status ((struct mtget *) buf);
  else if (cmd == MTIOCPOS)
    {
      ret = ERROR_INVALID_PARAMETER;
      if (buf && (ret = tape_get_pos (&block)))
	((struct mtpos *) buf)->mt_blkno = block;
    }
  else
    return fhandler_dev_raw::ioctl (cmd, buf);

  if (ret != NO_ERROR)
    {
      SetLastError (ret);
      __seterrno ();
      return -1;
    }

  return 0;
}

/* ------------------------------------------------------------------ */
/*                  Private functions used by `ioctl'                 */
/* ------------------------------------------------------------------ */

int
fhandler_dev_tape::tape_error (const char *txt)
{
  if (lasterr)
    debug_printf ("%s: error: %d", txt, lasterr);
  return lasterr;
}

int
fhandler_dev_tape::tape_write_marks (int marktype, DWORD len)
{
  syscall_printf ("write_tapemark");
  TAPE_FUNC (WriteTapemark (get_handle (), marktype, len, FALSE));
  return tape_error ("tape_write_marks");
}

int
fhandler_dev_tape::tape_get_pos (unsigned long *block,
				 unsigned long *partition)
{
  DWORD part, low, high;

  lasterr = ERROR_INVALID_PARAMETER;
  if (tape_get_feature (TAPE_DRIVE_GET_LOGICAL_BLK))
    TAPE_FUNC (GetTapePosition (get_handle (), TAPE_LOGICAL_POSITION,
				&part, &low, &high));
  else if (tape_get_feature (TAPE_DRIVE_GET_ABSOLUTE_BLK))
    TAPE_FUNC (GetTapePosition (get_handle (), TAPE_ABSOLUTE_POSITION,
				&part, &low, &high));

  if (!tape_error ("tape_get_pos"))
    {
      if (block)
	*block = low;
      if (partition)
        *partition = part > 0 ? part - 1 : part;
    }

  return lasterr;
}

int
fhandler_dev_tape::_tape_set_pos (int mode, long count, int partition)
{
  TAPE_FUNC (SetTapePosition (get_handle (), mode, partition, count,
			      count < 0 ? -1 : 0, FALSE));
  /* Reset buffer after successful repositioning. */
  if (!lasterr || IS_EOF (lasterr) || IS_EOM (lasterr))
    {
      reset_devbuf ();
      eof_detected = IS_EOF (lasterr);
      eom_detected = IS_EOM (lasterr);
    }
  return lasterr;
}

int
fhandler_dev_tape::tape_set_pos (int mode, long count, bool sfm_func)
{
  switch (mode)
    {
      case TAPE_SPACE_RELATIVE_BLOCKS:
      case TAPE_SPACE_FILEMARKS:
        if (!count)
	  {
	    lasterr = 0;
	    return tape_error ("tape_set_pos");
	  }
	break;
    }
  _tape_set_pos (mode, count);
  switch (mode)
    {
      case TAPE_SPACE_FILEMARKS:
	if (!lasterr && sfm_func)
	  return tape_set_pos (mode, count > 0 ? -1 : 1, false);
	break;
    }
  return tape_error ("tape_set_pos");
}

int
fhandler_dev_tape::tape_erase (int mode)
{
  if (tape_set_pos (TAPE_REWIND, 0))
    return lasterr;
  switch (mode)
    {
      case TAPE_ERASE_SHORT:
	if (!tape_get_feature (TAPE_DRIVE_ERASE_SHORT))
	  mode = TAPE_ERASE_LONG;
	break;
      case TAPE_ERASE_LONG:
	if (!tape_get_feature (TAPE_DRIVE_ERASE_LONG))
	  mode = TAPE_ERASE_SHORT;
	break;
    }
  TAPE_FUNC (EraseTape (get_handle (), mode, false));
  /* Reset buffer after successful tape erasing. */
  if (!lasterr)
    reset_devbuf ();
  return tape_error ("tape_erase");
}

int
fhandler_dev_tape::tape_prepare (int action)
{
  TAPE_FUNC (PrepareTape (get_handle (), action, FALSE));
  /* Reset buffer after all successful preparations but lock and unlock. */
  if (!lasterr && action != TAPE_LOCK && action != TAPE_UNLOCK)
    reset_devbuf ();
  return tape_error ("tape_prepare");
}

int
fhandler_dev_tape::tape_set_blocksize (long count)
{
  TAPE_SET_MEDIA_PARAMETERS mp;

  mp.BlockSize = count;
  TAPE_FUNC (SetTapeParameters (get_handle (), SET_TAPE_MEDIA_INFORMATION,
  				&mp));
  return tape_error ("tape_set_blocksize");
}

int
fhandler_dev_tape::tape_status (struct mtget *get)
{
  DWORD varlen;
  TAPE_GET_MEDIA_PARAMETERS mp;
  int notape = 0;

  if (!get)
    return ERROR_INVALID_PARAMETER;

  /* Setting varlen to sizeof DP is by intention, actually!  Never set
     it to sizeof MP which seems to be more correct but results in a
     ERROR_MORE_DATA error at least on W2K. */
  TAPE_FUNC (GetTapeParameters (get_handle (), GET_TAPE_MEDIA_INFORMATION,
				(varlen = sizeof dp, &varlen), &mp));
  if (lasterr)
    notape = 1;

  memset (get, 0, sizeof *get);

  get->mt_type = MT_ISUNKNOWN;

  if (!notape && tape_get_feature (TAPE_DRIVE_SET_BLOCK_SIZE))
    get->mt_dsreg = (mp.BlockSize << MT_ST_BLKSIZE_SHIFT)
		    & MT_ST_BLKSIZE_MASK;
  else
    get->mt_dsreg = (dp.DefaultBlockSize << MT_ST_BLKSIZE_SHIFT)
		    & MT_ST_BLKSIZE_MASK;

  if (wincap.has_ioctl_storage_get_media_types_ex ())
    {
      DWORD size = sizeof (GET_MEDIA_TYPES) + 10 * sizeof (DEVICE_MEDIA_INFO);
      void *buf = alloca (size);
      if (DeviceIoControl (get_handle (), IOCTL_STORAGE_GET_MEDIA_TYPES_EX,
			   NULL, 0, buf, size, &size, NULL)
	  || GetLastError () == ERROR_MORE_DATA)
	{
	  PGET_MEDIA_TYPES gmt = (PGET_MEDIA_TYPES) buf;
	  for (DWORD i = 0; i < gmt->MediaInfoCount; ++i)
	    {
	      PDEVICE_MEDIA_INFO dmi = &gmt->MediaInfo[i];
	      get->mt_type = dmi->DeviceSpecific.TapeInfo.MediaType;
#define TINFO DeviceSpecific.TapeInfo
	      if (dmi->TINFO.MediaCharacteristics & MEDIA_CURRENTLY_MOUNTED)
		{
		  get->mt_type = dmi->DeviceSpecific.TapeInfo.MediaType;
		  if (dmi->TINFO.BusType == BusTypeScsi)
		    get->mt_dsreg |=
		      (dmi->TINFO.BusSpecificData.ScsiInformation.DensityCode
		       << MT_ST_DENSITY_SHIFT)
		      & MT_ST_DENSITY_MASK;
		  break;
		}
#undef TINFO
	    }
	}
    }
  if (notape)
    get->mt_gstat |= GMT_DR_OPEN (-1);

  if (!notape)
    {
      if (tape_get_feature (TAPE_DRIVE_GET_LOGICAL_BLK)
          || tape_get_feature (TAPE_DRIVE_GET_ABSOLUTE_BLK))
	tape_get_pos ((unsigned long *) &get->mt_blkno,
		      (unsigned long *) &get->mt_resid);

      if (!get->mt_blkno)
	get->mt_gstat |= GMT_BOT (-1);

      get->mt_gstat |= GMT_ONLINE (-1);

      if (tape_get_feature (TAPE_DRIVE_WRITE_PROTECT) && mp.WriteProtected)
	get->mt_gstat |= GMT_WR_PROT (-1);

      if (tape_get_feature (TAPE_DRIVE_TAPE_CAPACITY))
	get->mt_capacity = get_ll (mp.Capacity);

      if (tape_get_feature (TAPE_DRIVE_TAPE_REMAINING))
	  get->mt_remaining = get_ll (mp.Remaining);
    }

  if (tape_get_feature (TAPE_DRIVE_COMPRESSION) && dp.Compression)
    get->mt_gstat |= GMT_HW_COMP (-1);

  if (tape_get_feature (TAPE_DRIVE_ECC) && dp.ECC)
    get->mt_gstat |= GMT_HW_ECC (-1);

  if (tape_get_feature (TAPE_DRIVE_PADDING) && dp.DataPadding)
    get->mt_gstat |= GMT_PADDING (-1);

  if (tape_get_feature (TAPE_DRIVE_REPORT_SMKS) && dp.ReportSetmarks)
    get->mt_gstat |= GMT_IM_REP_EN (-1);

  get->mt_erreg = lasterr;

  get->mt_minblksize = dp.MinimumBlockSize;
  get->mt_maxblksize = dp.MaximumBlockSize;
  get->mt_defblksize = dp.DefaultBlockSize;
  get->mt_featureslow = dp.FeaturesLow;
  get->mt_featureshigh = dp.FeaturesHigh;
  get->mt_eotwarningzonesize = dp.EOTWarningZoneSize;

  return 0;
}

int
fhandler_dev_tape::tape_compression (long count)
{
  TAPE_SET_DRIVE_PARAMETERS dps;

  if (!tape_get_feature (TAPE_DRIVE_COMPRESSION))
    return ERROR_INVALID_PARAMETER;

  dps.ECC = dp.ECC;
  dps.Compression = count ? TRUE : FALSE;
  dps.DataPadding = dp.DataPadding;
  dps.ReportSetmarks = dp.ReportSetmarks;
  dps.EOTWarningZoneSize = dp.EOTWarningZoneSize;
  TAPE_FUNC (SetTapeParameters (get_handle (), SET_TAPE_DRIVE_INFORMATION,
				&dps));
  if (!lasterr)
    dp.Compression = dps.Compression;
  return tape_error ("tape_compression");
}

int
fhandler_dev_tape::tape_partition (long count)
{
  if (dp.MaximumPartitionCount <= 1)
    return ERROR_INVALID_PARAMETER;
  if (tape_set_pos (TAPE_REWIND, 0))
    return lasterr;
  if (count <= 0)
    debug_printf ("Formatting tape with one partition");
  else
    debug_printf ("Formatting tape with two partitions");
  TAPE_FUNC (CreateTapePartition (get_handle (), TAPE_SELECT_PARTITIONS,
				   count <= 0 ? 1 : 2, 0));
  return tape_error ("tape_partition");
}

int
fhandler_dev_tape::tape_set_partition (long count)
{
  if (count < 0 || (unsigned long) count >= dp.MaximumPartitionCount)
    lasterr = ERROR_INVALID_PARAMETER;
  else
    lasterr = _tape_set_pos (TAPE_LOGICAL_BLOCK, 0, count + 1);
  return tape_error ("tape_set_partition");
}
