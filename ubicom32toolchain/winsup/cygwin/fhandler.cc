/* fhandler.cc.  See console.cc for fhandler_console functions.

   Copyright 1996, 1997, 1998, 1999, 2000, 2001, 2002, 2003, 2004,
   2005, 2006, 2007 Red Hat, Inc.

This file is part of Cygwin.

This software is a copyrighted work licensed under the terms of the
Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
details. */

#include "winsup.h"
#include <unistd.h>
#include <stdlib.h>
#include <sys/cygwin.h>
#include <sys/uio.h>
#include <sys/acl.h>
#include <sys/statvfs.h>
#include <signal.h>
#include "cygerrno.h"
#include "perprocess.h"
#include "security.h"
#include "cygwin/version.h"
#include "path.h"
#include "fhandler.h"
#include "dtable.h"
#include "cygheap.h"
#include "shared_info.h"
#include "pinfo.h"
#include <assert.h>
#include <limits.h>
#include <winioctl.h>
#include <ntdef.h>
#include "ntdll.h"
#include "cygtls.h"
#include "sigproc.h"

static NO_COPY const int CHUNK_SIZE = 1024; /* Used for crlf conversions */

struct __cygwin_perfile *perfile_table;

DWORD binmode;

inline fhandler_base&
fhandler_base::operator =(fhandler_base& x)
{
  memcpy (this, &x, sizeof *this);
  pc = x.pc;
  rabuf = NULL;
  ralen = 0;
  raixget = 0;
  raixput = 0;
  rabuflen = 0;
  return *this;
}

int
fhandler_base::puts_readahead (const char *s, size_t len)
{
  int success = 1;
  while ((len == (size_t) -1 ? *s : len--)
	 && (success = put_readahead (*s++) > 0))
    continue;
  return success;
}

int
fhandler_base::put_readahead (char value)
{
  char *newrabuf;
  if (raixput < rabuflen)
    /* Nothing to do */;
  else if ((newrabuf = (char *) realloc (rabuf, rabuflen += 32)))
    rabuf = newrabuf;
  else
    return 0;

  rabuf[raixput++] = value;
  ralen++;
  return 1;
}

int
fhandler_base::get_readahead ()
{
  int chret = -1;
  if (raixget < ralen)
    chret = ((unsigned char) rabuf[raixget++]) & 0xff;
  /* FIXME - not thread safe */
  if (raixget >= ralen)
    raixget = raixput = ralen = 0;
  return chret;
}

int
fhandler_base::peek_readahead (int queryput)
{
  int chret = -1;
  if (!queryput && raixget < ralen)
    chret = ((unsigned char) rabuf[raixget]) & 0xff;
  else if (queryput && raixput > 0)
    chret = ((unsigned char) rabuf[raixput - 1]) & 0xff;
  return chret;
}

void
fhandler_base::set_readahead_valid (int val, int ch)
{
  if (!val)
    ralen = raixget = raixput = 0;
  if (ch != -1)
    put_readahead (ch);
}

int
fhandler_base::eat_readahead (int n)
{
  int oralen = ralen;
  if (n < 0)
    n = ralen;
  if (n > 0 && ralen)
    {
      if ((int) (ralen -= n) < 0)
	ralen = 0;

      if (raixget >= ralen)
	raixget = raixput = ralen = 0;
      else if (raixput > ralen)
	raixput = ralen;
    }

  return oralen;
}

int
fhandler_base::get_readahead_into_buffer (char *buf, size_t buflen)
{
  int ch;
  int copied_chars = 0;

  while (buflen)
    if ((ch = get_readahead ()) < 0)
      break;
    else
      {
	buf[copied_chars++] = (unsigned char)(ch & 0xff);
	buflen--;
      }

  return copied_chars;
}

/* Record the file name. and name hash */
void
fhandler_base::set_name (path_conv &in_pc)
{
  pc = in_pc;
}

char *fhandler_base::get_proc_fd_name (char *buf)
{
  if (get_name ())
    return strcpy (buf, get_name ());
  if (dev ().name)
    return strcpy (buf, dev ().name);
  return strcpy (buf, "");
}

/* Detect if we are sitting at EOF for conditions where Windows
   returns an error but UNIX doesn't.  */
static int __stdcall
is_at_eof (HANDLE h, DWORD err)
{
  DWORD size, upper1, curr;

  size = GetFileSize (h, &upper1);
  if (size != INVALID_FILE_SIZE || GetLastError () == NO_ERROR)
    {
      LONG upper2 = 0;
      curr = SetFilePointer (h, 0, &upper2, FILE_CURRENT);
      if (curr == size && upper1 == (DWORD) upper2)
	return 1;
    }

  SetLastError (err);
  return 0;
}

void
fhandler_base::set_flags (int flags, int supplied_bin)
{
  int bin;
  int fmode;
  debug_printf ("flags %p, supplied_bin %p", flags, supplied_bin);
  if ((bin = flags & (O_BINARY | O_TEXT)))
    debug_printf ("O_TEXT/O_BINARY set in flags %p", bin);
  else if (rbinset () && wbinset ())
    bin = rbinary () ? O_BINARY : O_TEXT;	// FIXME: Not quite right
  else if ((fmode = get_default_fmode (flags)) & O_BINARY)
    bin = O_BINARY;
  else if (fmode & O_TEXT)
    bin = O_TEXT;
  else if (supplied_bin)
    bin = supplied_bin;
  else
    bin = wbinary () || rbinary () || (binmode != O_TEXT)
	  ? O_BINARY : O_TEXT;

  openflags = flags | bin;

  bin &= O_BINARY;
  rbinary (bin ? true : false);
  wbinary (bin ? true : false);
  syscall_printf ("filemode set to %s", bin ? "binary" : "text");
}

/* Normal file i/o handlers.  */

/* Cover function to ReadFile to achieve (as much as possible) Posix style
   semantics and use of errno.  */
void
fhandler_base::raw_read (void *ptr, size_t& ulen)
{
#define bytes_read ulen

  int try_noreserve = 1;
  DWORD len = ulen;

retry:
  ulen = (size_t) -1;
  BOOL res = ReadFile (get_handle (), ptr, len, (DWORD *) &ulen, NULL);
  if (!res)
    {
      /* Some errors are not really errors.  Detect such cases here.  */

      DWORD  errcode = GetLastError ();
      switch (errcode)
	{
	case ERROR_BROKEN_PIPE:
	  /* This is really EOF.  */
	  bytes_read = 0;
	  break;
	case ERROR_MORE_DATA:
	  /* `bytes_read' is supposedly valid.  */
	  break;
	case ERROR_NOACCESS:
	  if (is_at_eof (get_handle (), errcode))
	    {
	      bytes_read = 0;
	      break;
	    }
	  if (try_noreserve)
	    {
	      try_noreserve = 0;
	      switch (mmap_is_attached_or_noreserve (ptr, len))
		{
		case MMAP_NORESERVE_COMMITED:
		  goto retry;
		case MMAP_RAISE_SIGBUS:
		  raise(SIGBUS);
		case MMAP_NONE:
		  break;
		}
	    }
	  /*FALLTHRU*/
	case ERROR_INVALID_FUNCTION:
	case ERROR_INVALID_PARAMETER:
	case ERROR_INVALID_HANDLE:
	  if (pc.isdir ())
	    {
	      set_errno (EISDIR);
	      bytes_read = (size_t) -1;
	      break;
	    }
	default:
	  syscall_printf ("ReadFile %s(%p) failed, %E", get_name (), get_handle ());
	  __seterrno_from_win_error (errcode);
	  bytes_read = (size_t) -1;
	  break;
	}
    }
#undef bytes_read
}

/* Cover function to WriteFile to provide Posix interface and semantics
   (as much as possible).  */
int
fhandler_base::raw_write (const void *ptr, size_t len)
{
  DWORD bytes_written;

  if (!WriteFile (get_output_handle (), ptr, len, &bytes_written, 0))
    {
      if (GetLastError () == ERROR_DISK_FULL && bytes_written > 0)
	goto written;
      __seterrno ();
      if (get_errno () == EPIPE)
	raise (SIGPIPE);
      return -1;
    }
written:
  return bytes_written;
}

int
fhandler_base::get_default_fmode (int flags)
{
  int fmode = __fmode;
  if (perfile_table)
    {
      size_t nlen = strlen (get_name ());
      unsigned accflags = (flags & O_ACCMODE);
      for (__cygwin_perfile *pf = perfile_table; pf->name; pf++)
	if (!*pf->name && (pf->flags & O_ACCMODE) == accflags)
	  {
	    fmode = pf->flags & ~O_ACCMODE;
	    break;
	  }
	else
	  {
	    size_t pflen = strlen (pf->name);
	    const char *stem = get_name () + nlen - pflen;
	    if (pflen > nlen || (stem != get_name () && !isdirsep (stem[-1])))
	      continue;
	    else if ((pf->flags & O_ACCMODE) == accflags
		     && strcasematch (stem, pf->name))
	      {
		fmode = pf->flags & ~O_ACCMODE;
		break;
	      }
	  }
    }
  return fmode;
}

bool
fhandler_base::device_access_denied (int flags)
{
  int mode = 0;

  if (flags & O_RDWR)
    mode |= R_OK | W_OK;
  if (flags & (O_WRONLY | O_APPEND))
    mode |= W_OK;
  if (!mode)
    mode |= R_OK;

  return fhaccess (mode);
}

int
fhandler_base::fhaccess (int flags)
{
  int res = -1;
  if (error ())
    {
      set_errno (error ());
      goto done;
    }

  if (!exists ())
    {
      set_errno (ENOENT);
      goto done;
    }

  if (!(flags & (R_OK | W_OK | X_OK)))
    return 0;

  if (is_fs_special ())
    /* short circuit */;
  else if (has_attribute (FILE_ATTRIBUTE_READONLY) && (flags & W_OK))
    goto eaccess_done;
  else if (has_acls () && allow_ntsec)
    {
      res = check_file_access (pc, flags);
      goto done;
    }
  else if (get_device () == FH_REGISTRY && allow_ntsec && open (O_RDONLY, 0)
	   && get_handle ())
    {
      res = check_registry_access (get_handle (), flags);
      close ();
      return res;
    }

  struct __stat64 st;
  if (fstat (&st))
    goto done;

  if (flags & R_OK)
    {
      if (st.st_uid == myself->uid)
	{
	  if (!(st.st_mode & S_IRUSR))
	    goto eaccess_done;
	}
      else if (st.st_gid == myself->gid)
	{
	  if (!(st.st_mode & S_IRGRP))
	    goto eaccess_done;
	}
      else if (!(st.st_mode & S_IROTH))
	goto eaccess_done;
    }

  if (flags & W_OK)
    {
      if (st.st_uid == myself->uid)
	{
	  if (!(st.st_mode & S_IWUSR))
	    goto eaccess_done;
	}
      else if (st.st_gid == myself->gid)
	{
	  if (!(st.st_mode & S_IWGRP))
	    goto eaccess_done;
	}
      else if (!(st.st_mode & S_IWOTH))
	goto eaccess_done;
    }

  if (flags & X_OK)
    {
      if (st.st_uid == myself->uid)
	{
	  if (!(st.st_mode & S_IXUSR))
	    goto eaccess_done;
	}
      else if (st.st_gid == myself->gid)
	{
	  if (!(st.st_mode & S_IXGRP))
	    goto eaccess_done;
	}
      else if (!(st.st_mode & S_IXOTH))
	goto eaccess_done;
    }

  res = 0;
  goto done;

eaccess_done:
  set_errno (EACCES);
done:
  if (!res && (flags & W_OK) && get_device () == FH_FS
      && (pc.fs_flags () & FILE_READ_ONLY_VOLUME))
    {
      set_errno (EROFS);
      res = -1;
    }
  debug_printf ("returning %d", res);
  return res;
}

/* Open system call handler function. */
int
fhandler_base::open (int flags, mode_t mode)
{
  int res = 0;
  HANDLE x;
  ULONG file_attributes = 0;
  ULONG shared = (get_major () == DEV_TAPE_MAJOR ? 0 : FILE_SHARE_VALID_FLAGS);
  ULONG create_disposition;
  ULONG create_options;
  SECURITY_ATTRIBUTES sa = sec_none;
  security_descriptor sd;
  OBJECT_ATTRIBUTES attr;
  IO_STATUS_BLOCK io;
  NTSTATUS status;

  syscall_printf ("(%S, %p)", pc.get_nt_native_path (), flags);

  pc.get_object_attr (attr, sa);

  switch (query_open ())
    {
      case query_read_control:
	access = READ_CONTROL;
	create_options = FILE_OPEN_FOR_BACKUP_INTENT;
	break;
      case query_read_attributes:
	access = READ_CONTROL | FILE_READ_ATTRIBUTES;
	create_options = FILE_OPEN_FOR_BACKUP_INTENT;
	break;
      case query_write_control:
	access = READ_CONTROL | WRITE_OWNER | WRITE_DAC | FILE_WRITE_ATTRIBUTES;
	create_options = FILE_OPEN_FOR_BACKUP_INTENT | FILE_OPEN_FOR_RECOVERY;
	break;
      case query_write_attributes:
	access = READ_CONTROL | FILE_WRITE_ATTRIBUTES;
	create_options = FILE_OPEN_FOR_BACKUP_INTENT | FILE_OPEN_FOR_RECOVERY;
	break;
      default:
	if ((flags & O_ACCMODE) == O_RDONLY)
	  {
	    access = GENERIC_READ;
	    create_options = FILE_OPEN_FOR_BACKUP_INTENT;
	  }
	else if ((flags & O_ACCMODE) == O_WRONLY)
	  {
	    access = GENERIC_WRITE | READ_CONTROL | FILE_READ_ATTRIBUTES;
	    create_options = FILE_OPEN_FOR_BACKUP_INTENT
			     | FILE_OPEN_FOR_RECOVERY;
	  }
	else
	  {
	    access = GENERIC_READ | GENERIC_WRITE;
	    create_options = FILE_OPEN_FOR_BACKUP_INTENT
			     | FILE_OPEN_FOR_RECOVERY;
	  }
	if (flags & O_SYNC)
	  create_options |= FILE_WRITE_THROUGH;
	if (flags & O_DIRECT)
	  create_options |= FILE_NO_INTERMEDIATE_BUFFERING;
	if (get_major () != DEV_SERIAL_MAJOR && get_major () != DEV_TAPE_MAJOR)
	  {
	    create_options |= FILE_SYNCHRONOUS_IO_NONALERT;
	    access |= SYNCHRONIZE;
	  }
	break;
    }

  if ((flags & O_TRUNC) && ((flags & O_ACCMODE) != O_RDONLY))
    {
      if (flags & O_CREAT)
	create_disposition = FILE_OVERWRITE_IF;
      else
	create_disposition = FILE_OVERWRITE;
    }
  else if (flags & O_CREAT)
    create_disposition = FILE_OPEN_IF;
  else
    create_disposition = FILE_OPEN;

  if ((flags & O_EXCL) && (flags & O_CREAT))
    create_disposition = FILE_CREATE;

  if (flags & O_CREAT && get_device () == FH_FS)
    {
      file_attributes = FILE_ATTRIBUTE_NORMAL;
      /* If mode has no write bits set, we set the R/O attribute. */
      if (!(mode & (S_IWUSR | S_IWGRP | S_IWOTH)))
	file_attributes |= FILE_ATTRIBUTE_READONLY;
      /* Starting with Windows 2000, when trying to overwrite an already
	 existing file with FILE_ATTRIBUTE_HIDDEN and/or FILE_ATTRIBUTE_SYSTEM
	 attribute set, CreateFile fails with ERROR_ACCESS_DENIED.
	 Per MSDN you have to create the file with the same attributes as
	 already specified for the file. */
      if (has_attribute (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
	file_attributes |= pc.file_attributes ();

      /* If the file should actually be created and ntsec is on,
	 set files attributes. */
      /* TODO: Don't remove the call to has_acls() unless there's a
	 solution for the security descriptor problem on remote samba
	 drives.  The local user SID is used in set_security_attribute,
	 but the actual owner on the Samba share is the SID of the Unix
	 account.  There's no transparent mapping between these accounts.
	 And Samba has a strange behaviour when creating a file.  Apparently
	 it *first*( creates the file, *then* it looks if the security
	 descriptor matches.  The result is that the file gets created, but
	 then NtCreateFile doesn't return a handle to the file and fails
	 with STATUS_ACCESS_DENIED.  Go figure! */
      if (allow_ntsec && has_acls ())
	{
	  set_security_attribute (mode, &sa, sd);
	  attr.SecurityDescriptor = sa.lpSecurityDescriptor;
	}
      /* The file attributes are needed for later use in, e.g. fchmod. */
      pc.file_attributes (file_attributes);
    }

  status = NtCreateFile (&x, access, &attr, &io, NULL, file_attributes, shared,
			 create_disposition, create_options, NULL, 0);
  if (!NT_SUCCESS (status))
    {
      __seterrno_from_nt_status (status);
      if (!nohandle ())
	goto done;
   }

  set_io_handle (x);
  set_flags (flags, pc.binmode ());

  res = 1;
  set_open_status ();
done:
  debug_printf ("%x = NtCreateFile "
		"(%p, %x, %S, io, NULL, %x, %x, %x, %x, NULL, 0)",
		status, x, access, pc.get_nt_native_path (), file_attributes,
		shared, create_disposition, create_options);

  syscall_printf ("%d = fhandler_base::open (%s, %p)",
		  res, pc.get_nt_native_path (), flags);
  return res;
}

/* states:
   open buffer in binary mode?  Just do the read.

   open buffer in text mode?  Scan buffer for control zs and handle
   the first one found.  Then scan buffer, converting every \r\n into
   an \n.  If last char is an \r, look ahead one more char, if \n then
   modify \r, if not, remember char.
*/
void
fhandler_base::read (void *in_ptr, size_t& len)
{
  char *ptr = (char *) in_ptr;
  ssize_t copied_chars = get_readahead_into_buffer (ptr, len);

  if (copied_chars && is_slow ())
    {
      len = (size_t) copied_chars;
      goto out;
    }

  len -= copied_chars;
  if (!len)
    {
      len = (size_t) copied_chars;
      goto out;
    }

  raw_read (ptr + copied_chars, len);
  if (!copied_chars)
    /* nothing */;
  else if ((ssize_t) len > 0)
    len += copied_chars;
  else
    len = copied_chars;

  if (rbinary () || len <= 0)
    goto out;

  /* Scan buffer and turn \r\n into \n */
  char *src, *dst, *end;
  src = (char *) ptr;
  dst = (char *) ptr;
  end = src + len - 1;

  /* Read up to the last but one char - the last char needs special handling */
  while (src < end)
    {
      if (*src == '\r' && src[1] == '\n')
	src++;
      *dst++ = *src++;
    }

  /* If not beyond end and last char is a '\r' then read one more
     to see if we should translate this one too */
  if (src > end)
    /* nothing */;
  else if (*src != '\r')
    *dst++ = *src;
  else
    {
      char c1;
      size_t c1len = 1;
      raw_read (&c1, c1len);
      if (c1len <= 0)
	/* nothing */;
      else if (c1 == '\n')
	*dst++ = '\n';
      else
	{
	  set_readahead_valid (1, c1);
	  *dst++ = *src;
	}
    }

  len = dst - (char *) ptr;

#ifndef NOSTRACE
  if (strace.active ())
    {
      char buf[16 * 6 + 1];
      char *p = buf;

      for (int i = 0; i < copied_chars && i < 16; ++i)
	{
	  unsigned char c = ((unsigned char *) ptr)[i];
	  __small_sprintf (p, " %c", c);
	  p += strlen (p);
	}
      *p = '\0';
      debug_printf ("read %d bytes (%s%s)", copied_chars, buf,
		    copied_chars > 16 ? " ..." : "");
    }
#endif

out:
  debug_printf ("returning %d, %s mode", len, rbinary () ? "binary" : "text");
}

int
fhandler_base::write (const void *ptr, size_t len)
{
  int res;

  if (get_flags () & O_APPEND)
    {
      LONG off_high = 0;
      DWORD ret = SetFilePointer (get_output_handle (), 0, &off_high, FILE_END);
      if (ret == INVALID_SET_FILE_POINTER && GetLastError () != NO_ERROR)
	{
	  debug_printf ("Seeking to EOF in append mode failed");
	  __seterrno ();
	  return -1;
	}
    }
  else if (did_lseek ())
    {
      _off64_t actual_length, current_position;
      DWORD size_high = 0;
      LONG pos_high = 0;

      did_lseek (false); /* don't do it again */

      actual_length = GetFileSize (get_output_handle (), &size_high);
      actual_length += ((_off64_t) size_high) << 32;

      current_position = SetFilePointer (get_output_handle (), 0, &pos_high,
					 FILE_CURRENT);
      current_position += ((_off64_t) pos_high) << 32;

      if (current_position >= actual_length + (128 * 1024)
	  && get_fs_flags (FILE_SUPPORTS_SPARSE_FILES))
	{
	  /* If the file system supports sparse files and the application
	     is writing after a long seek beyond EOF, convert the file to
	     a sparse file. */
	  DWORD dw;
	  HANDLE h = get_output_handle ();
	  BOOL r = DeviceIoControl (h, FSCTL_SET_SPARSE, NULL, 0, NULL,
				    0, &dw, NULL);
	  syscall_printf ("%d = DeviceIoControl(%p, FSCTL_SET_SPARSE)",
			  r, h);
	}
    }

  if (wbinary ())
    {
      debug_printf ("binary write");
      res = raw_write (ptr, len);
    }
  else
    {
      debug_printf ("text write");
      /* This is the Microsoft/DJGPP way.  Still not ideal, but it's
	 compatible.
	 Modified slightly by CGF 2000-10-07 */

      int left_in_data = len;
      char *data = (char *)ptr;
      res = 0;

      while (left_in_data > 0)
	{
	  char buf[CHUNK_SIZE + 1], *buf_ptr = buf;
	  int left_in_buf = CHUNK_SIZE;

	  while (left_in_buf > 0 && left_in_data > 0)
	    {
	      char ch = *data++;
	      if (ch == '\n')
		{
		  *buf_ptr++ = '\r';
		  left_in_buf--;
		}
	      *buf_ptr++ = ch;
	      left_in_buf--;
	      left_in_data--;
	      if (left_in_data > 0 && ch == '\r' && *data == '\n')
		{
		  *buf_ptr++ = *data++;
		  left_in_buf--;
		  left_in_data--;
		}
	    }

	  /* We've got a buffer-full, or we're out of data.  Write it out */
	  int nbytes;
	  int want = buf_ptr - buf;
	  if ((nbytes = raw_write (buf, want)) == want)
	    {
	      /* Keep track of how much written not counting additional \r's */
	      res = data - (char *)ptr;
	      continue;
	    }

	  if (nbytes == -1)
	    res = -1;		/* Error */
	  else
	    res += nbytes;	/* Partial write.  Return total bytes written. */
	  break;		/* All done */
	}
    }

  return res;
}

ssize_t
fhandler_base::readv (const struct iovec *const iov, const int iovcnt,
		      ssize_t tot)
{
  assert (iov);
  assert (iovcnt >= 1);

  size_t len = tot;
  if (iovcnt == 1)
    {
      len = iov->iov_len;
      read (iov->iov_base, len);
      return len;
    }

  if (tot == -1)		// i.e. if not pre-calculated by the caller.
    {
      len = 0;
      const struct iovec *iovptr = iov + iovcnt;
      do
	{
	  iovptr -= 1;
	  len += iovptr->iov_len;
	}
      while (iovptr != iov);
    }

  if (!len)
    return 0;

  char *buf = (char *) malloc (len);

  if (!buf)
    {
      set_errno (ENOMEM);
      return -1;
    }

  read (buf, len);
  ssize_t nbytes = (ssize_t) len;

  const struct iovec *iovptr = iov;

  char *p = buf;
  while (nbytes > 0)
    {
      const int frag = min (nbytes, (ssize_t) iovptr->iov_len);
      memcpy (iovptr->iov_base, p, frag);
      p += frag;
      iovptr += 1;
      nbytes -= frag;
    }

  free (buf);
  return len;
}

ssize_t
fhandler_base::writev (const struct iovec *const iov, const int iovcnt,
		       ssize_t tot)
{
  assert (iov);
  assert (iovcnt >= 1);

  if (iovcnt == 1)
    return write (iov->iov_base, iov->iov_len);

  if (tot == -1)		// i.e. if not pre-calculated by the caller.
    {
      tot = 0;
      const struct iovec *iovptr = iov + iovcnt;
      do
	{
	  iovptr -= 1;
	  tot += iovptr->iov_len;
	}
      while (iovptr != iov);
    }

  assert (tot >= 0);

  if (tot == 0)
    return 0;

  char *const buf = (char *) malloc (tot);

  if (!buf)
    {
      set_errno (ENOMEM);
      return -1;
    }

  char *bufptr = buf;
  const struct iovec *iovptr = iov;
  int nbytes = tot;

  while (nbytes != 0)
    {
      const int frag = min (nbytes, (ssize_t) iovptr->iov_len);
      memcpy (bufptr, iovptr->iov_base, frag);
      bufptr += frag;
      iovptr += 1;
      nbytes -= frag;
    }
  ssize_t ret = write (buf, tot);
  free (buf);
  return ret;
}

_off64_t
fhandler_base::lseek (_off64_t offset, int whence)
{
  _off64_t res;

  /* Seeks on text files is tough, we rewind and read till we get to the
     right place.  */

  if (whence != SEEK_CUR || offset != 0)
    {
      if (whence == SEEK_CUR)
	offset -= ralen - raixget;
      set_readahead_valid (0);
    }

  debug_printf ("lseek (%s, %D, %d)", get_name (), offset, whence);

  DWORD win32_whence = whence == SEEK_SET ? FILE_BEGIN
		       : (whence == SEEK_CUR ? FILE_CURRENT : FILE_END);

  LONG off_low = ((__uint64_t) offset) & UINT32_MAX;
  LONG off_high = ((__uint64_t) offset) >> 32LL;

  debug_printf ("setting file pointer to %u (high), %u (low)", off_high, off_low);
  res = SetFilePointer (get_handle (), off_low, &off_high, win32_whence);
  if (res == INVALID_SET_FILE_POINTER && GetLastError ())
    {
      __seterrno ();
      res = -1;
    }
  else
    {
      res += (_off64_t) off_high << 32;

      /* When next we write(), we will check to see if *this* seek went beyond
	 the end of the file and if so, potentially sparsify the file. */
      did_lseek (true);

      /* If this was a SEEK_CUR with offset 0, we still might have
	 readahead that we have to take into account when calculating
	 the actual position for the application.  */
      if (whence == SEEK_CUR)
	res -= ralen - raixget;
    }

  return res;
}

ssize_t __stdcall
fhandler_base::pread (void *, size_t, _off64_t)
{
  set_errno (ESPIPE);
  return -1;
}

ssize_t __stdcall
fhandler_base::pwrite (void *, size_t, _off64_t)
{
  set_errno (ESPIPE);
  return -1;
}

int
fhandler_base::close ()
{
  int res = -1;

  syscall_printf ("closing '%s' handle %p", get_name (), get_handle ());
  if (nohandle () || CloseHandle (get_handle ()))
    res = 0;
  else
    {
      paranoid_printf ("CloseHandle (%d <%s>) failed", get_handle (),
		       get_name ());

      __seterrno ();
    }
  destroy_overlapped ();
  return res;
}

int
fhandler_base::ioctl (unsigned int cmd, void *buf)
{
  int res;

  switch (cmd)
    {
    case FIONBIO:
      set_nonblocking (*(int *) buf);
      res = 0;
      break;
    default:
      set_errno (EINVAL);
      res = -1;
      break;
    }

  syscall_printf ("%d = ioctl (%x, %p)", res, cmd, buf);
  return res;
}

int
fhandler_base::lock (int, struct __flock64 *)
{
  set_errno (EINVAL);
  return -1;
}

int __stdcall
fhandler_base::fstat (struct __stat64 *buf)
{
  debug_printf ("here");

  if (is_fs_special ())
    return fstat_fs (buf);

  switch (get_device ())
    {
    case FH_PIPE:
      buf->st_mode = S_IFIFO | S_IRUSR | S_IWUSR;
      break;
    case FH_PIPEW:
      buf->st_mode = S_IFIFO | S_IWUSR;
      break;
    case FH_PIPER:
      buf->st_mode = S_IFIFO | S_IRUSR;
      break;
    case FH_FULL:
      buf->st_mode = S_IFCHR | S_IRUSR | S_IWUSR | S_IWGRP | S_IWOTH;
      break;
    default:
      buf->st_mode = S_IFCHR | STD_RBITS | STD_WBITS | S_IWGRP | S_IWOTH;
      break;
    }

  buf->st_uid = geteuid32 ();
  buf->st_gid = getegid32 ();
  buf->st_nlink = 1;
  buf->st_blksize = PREFERRED_IO_BLKSIZE;
  buf->st_ctim.tv_sec = 1164931200L;	/* Arbitrary value: 2006-12-01 */
  buf->st_ctim.tv_nsec = 0L;
  buf->st_atim = buf->st_mtim = buf->st_birthtim = buf->st_ctim;
  return 0;
}

int __stdcall
fhandler_base::fstatvfs (struct statvfs *sfs)
{
  /* If we hit this base implementation, it's some device in /dev.
     Just call statvfs on /dev for simplicity. */
  path_conv pc ("/dev");
  fhandler_disk_file fh (pc);
  return fh.fstatvfs (sfs);
}

void
fhandler_base::init (HANDLE f, DWORD a, mode_t bin)
{
  set_io_handle (f);
  access = a;
  a &= GENERIC_READ | GENERIC_WRITE;
  int flags = 0;
  if (a == GENERIC_READ)
    flags = O_RDONLY;
  else if (a == GENERIC_WRITE)
    flags = O_WRONLY;
  else if (a == (GENERIC_READ | GENERIC_WRITE))
    flags = O_RDWR;
  set_flags (flags | bin);
  set_open_status ();
  debug_printf ("created new fhandler_base for handle %p, bin %d", f, rbinary ());
}

int
fhandler_base::dup (fhandler_base *child)
{
  debug_printf ("in fhandler_base dup");

  HANDLE nh;
  if (!nohandle ())
    {
      if (!DuplicateHandle (hMainProc, get_handle (), hMainProc, &nh, 0, TRUE,
			    DUPLICATE_SAME_ACCESS))
	{
	  debug_printf ("dup(%s) failed, handle %x, %E",
			get_name (), get_handle ());
	  __seterrno ();
	  return -1;
	}

      VerifyHandle (nh);
      child->set_io_handle (nh);
    }
  if (get_overlapped ())
    child->setup_overlapped ();
  set_flags (child->get_flags ());
  return 0;
}

int fhandler_base::fcntl (int cmd, void *arg)
{
  int res;

  switch (cmd)
    {
    case F_GETFD:
      res = close_on_exec () ? FD_CLOEXEC : 0;
      break;
    case F_SETFD:
      set_close_on_exec (((int) arg & FD_CLOEXEC) ? 1 : 0);
      res = 0;
      break;
    case F_GETFL:
      res = get_flags ();
      debug_printf ("GETFL: %p", res);
      break;
    case F_SETFL:
      {
	/*
	 * Only O_APPEND, O_ASYNC and O_NONBLOCK/O_NDELAY are allowed.
	 * Each other flag will be ignored.
	 * Since O_ASYNC isn't defined in fcntl.h it's currently
	 * ignored as well.
	 */
	const int allowed_flags = O_APPEND | O_NONBLOCK_MASK;
	int new_flags = (int) arg & allowed_flags;
	/* Carefully test for the O_NONBLOCK or deprecated OLD_O_NDELAY flag.
	   Set only the flag that has been passed in.  If both are set, just
	   record O_NONBLOCK.   */
	if ((new_flags & OLD_O_NDELAY) && (new_flags & O_NONBLOCK))
	  new_flags &= ~OLD_O_NDELAY;
	set_flags ((get_flags () & ~allowed_flags) | new_flags);
      }
      res = 0;
      break;
    case F_GETLK:
    case F_SETLK:
    case F_SETLKW:
      res = lock (cmd, (struct __flock64 *) arg);
      break;
    default:
      set_errno (EINVAL);
      res = -1;
      break;
    }
  return res;
}

/* Base terminal handlers.  These just return errors.  */

int
fhandler_base::tcflush (int)
{
  set_errno (ENOTTY);
  return -1;
}

int
fhandler_base::tcsendbreak (int)
{
  set_errno (ENOTTY);
  return -1;
}

int
fhandler_base::tcdrain ()
{
  set_errno (ENOTTY);
  return -1;
}

int
fhandler_base::tcflow (int)
{
  set_errno (ENOTTY);
  return -1;
}

int
fhandler_base::tcsetattr (int, const struct termios *)
{
  set_errno (ENOTTY);
  return -1;
}

int
fhandler_base::tcgetattr (struct termios *)
{
  set_errno (ENOTTY);
  return -1;
}

int
fhandler_base::tcsetpgrp (const pid_t)
{
  set_errno (ENOTTY);
  return -1;
}

int
fhandler_base::tcgetpgrp ()
{
  set_errno (ENOTTY);
  return -1;
}

void
fhandler_base::operator delete (void *p)
{
  cfree (p);
}

/* Normal I/O constructor */
fhandler_base::fhandler_base () :
  status (),
  open_status (),
  access (0),
  io_handle (NULL),
  namehash (0),
  openflags (0),
  rabuf (NULL),
  ralen (0),
  raixget (0),
  raixput (0),
  rabuflen (0),
  fs_flags (0),
  archetype (NULL),
  usecount (0)
{
}

/* Normal I/O destructor */
fhandler_base::~fhandler_base ()
{
  if (rabuf)
    free (rabuf);
}

/**********************************************************************/
/* /dev/null */

fhandler_dev_null::fhandler_dev_null () :
	fhandler_base ()
{
}

void
fhandler_base::set_no_inheritance (HANDLE &h, bool not_inheriting)
{
  if (!SetHandleInformation (h, HANDLE_FLAG_INHERIT,
			     not_inheriting ? 0 : HANDLE_FLAG_INHERIT))
    debug_printf ("SetHandleInformation failed, %E");
#ifdef DEBUGGING_AND_FDS_PROTECTED
  if (h)
    setclexec (oh, h, not_inheriting);
#endif
}

bool
fhandler_base::fork_fixup (HANDLE parent, HANDLE &h, const char *name)
{
  HANDLE oh = h;
  bool res = false;
  if (/* !is_socket () && */ !close_on_exec ())
    debug_printf ("handle %p already opened", h);
  else if (!DuplicateHandle (parent, h, hMainProc, &h, 0, !close_on_exec (),
			     DUPLICATE_SAME_ACCESS))
    system_printf ("%s - %E, handle %s<%p>", get_name (), name, h);
  else
    {
      if (oh != h)
	VerifyHandle (h);
      res = true;
    }
  if (get_overlapped ())
    setup_overlapped ();
  return res;
}

void
fhandler_base::set_close_on_exec (bool val)
{
  if (!nohandle ())
    set_no_inheritance (io_handle, val);
  close_on_exec (val);
  debug_printf ("set close_on_exec for %s to %d", get_name (), val);
}

void
fhandler_base::fixup_after_fork (HANDLE parent)
{
  debug_printf ("inheriting '%s' from parent", get_name ());
  if (!nohandle ())
    fork_fixup (parent, io_handle, "io_handle");
  if (get_overlapped ())
    setup_overlapped ();
}

void
fhandler_base::fixup_after_exec ()
{
  debug_printf ("here for '%s'", get_name ());
  if (get_overlapped ())
    setup_overlapped ();
}

bool
fhandler_base::is_nonblocking ()
{
  return (openflags & O_NONBLOCK_MASK) != 0;
}

void
fhandler_base::set_nonblocking (int yes)
{
  int current = openflags & O_NONBLOCK_MASK;
  int new_flags = yes ? (!current ? O_NONBLOCK : current) : 0;
  openflags = (openflags & ~O_NONBLOCK_MASK) | new_flags;
}

int
fhandler_base::mkdir (mode_t)
{
  if (exists ())
    set_errno (EEXIST);
  else
    set_errno (EROFS);
  return -1;
}

int
fhandler_base::rmdir ()
{
  if (!exists ())
    set_errno (ENOENT);
  else if (!pc.isdir ())
    set_errno (ENOTDIR);
  else
    set_errno (EROFS);
  return -1;
}

DIR *
fhandler_base::opendir (int fd)
{
  set_errno (ENOTDIR);
  return NULL;
}

int
fhandler_base::readdir (DIR *, dirent *)
{
  return ENOTDIR;
}

_off64_t
fhandler_base::telldir (DIR *)
{
  set_errno (ENOTDIR);
  return -1;
}

void
fhandler_base::seekdir (DIR *, _off64_t)
{
  set_errno (ENOTDIR);
}

void
fhandler_base::rewinddir (DIR *)
{
  set_errno (ENOTDIR);
}

int
fhandler_base::closedir (DIR *)
{
  set_errno (ENOTDIR);
  return -1;
}

int
fhandler_base::fchmod (mode_t mode)
{
  extern int chmod_device (path_conv& pc, mode_t mode);
  if (pc.is_fs_special ())
    return chmod_device (pc, mode);
  /* By default, just succeeds. */
  return 0;
}

int
fhandler_base::fchown (__uid32_t uid, __gid32_t gid)
{
  if (pc.is_fs_special ())
    return ((fhandler_disk_file *) this)->fhandler_disk_file::fchown (uid, gid);
  /* By default, just succeeds. */
  return 0;
}

int
fhandler_base::facl (int cmd, int nentries, __aclent32_t *aclbufp)
{
  int res = -1;
  switch (cmd)
    {
      case SETACL:
	/* By default, just succeeds. */
	res = 0;
	break;
      case GETACL:
	if (!aclbufp)
	  set_errno(EFAULT);
	else if (nentries < MIN_ACL_ENTRIES)
	  set_errno (ENOSPC);
	else
	  {
	    aclbufp[0].a_type = USER_OBJ;
	    aclbufp[0].a_id = myself->uid;
	    aclbufp[0].a_perm = (S_IRUSR | S_IWUSR) >> 6;
	    aclbufp[1].a_type = GROUP_OBJ;
	    aclbufp[1].a_id = myself->gid;
	    aclbufp[1].a_perm = (S_IRGRP | S_IWGRP) >> 3;
	    aclbufp[2].a_type = OTHER_OBJ;
	    aclbufp[2].a_id = ILLEGAL_GID;
	    aclbufp[2].a_perm = S_IROTH | S_IWOTH;
	    aclbufp[3].a_type = CLASS_OBJ;
	    aclbufp[3].a_id = ILLEGAL_GID;
	    aclbufp[3].a_perm = S_IRWXU | S_IRWXG | S_IRWXO;
	    res = MIN_ACL_ENTRIES;
	  }
	break;
      case GETACLCNT:
	res = MIN_ACL_ENTRIES;
	break;
      default:
	set_errno (EINVAL);
	break;
    }
  return res;
}

int
fhandler_base::fadvise (_off64_t offset, _off64_t length, int advice)
{
  set_errno (EINVAL);
  return -1;
}

int
fhandler_base::ftruncate (_off64_t length, bool allow_truncate)
{
  set_errno (EINVAL);
  return -1;
}

int
fhandler_base::link (const char *newpath)
{
  set_errno (EINVAL);
  return -1;
}

int
fhandler_base::utimes (const struct timeval *tvp)
{
  if (is_fs_special ())
    return utimes_fs (tvp);

  set_errno (EINVAL);
  return -1;
}

int
fhandler_base::fsync ()
{
  if (!get_handle () || nohandle ())
    {
      set_errno (EINVAL);
      return -1;
    }
  if (pc.isdir ()) /* Just succeed. */
    return 0;
  if (FlushFileBuffers (get_handle ()))
    return 0;
  __seterrno ();
  return -1;
}

int
fhandler_base::fpathconf (int v)
{
  switch (v)
    {
    case _PC_LINK_MAX:
      return pc.fs_is_ntfs () || pc.fs_is_samba () || pc.fs_is_nfs ()
	     ? LINK_MAX : 1;
    case _PC_MAX_CANON:
      if (is_tty ())
	return MAX_CANON;
      set_errno (EINVAL);
      break;
    case _PC_MAX_INPUT:
      if (is_tty ())
	return MAX_INPUT;
      set_errno (EINVAL);
      break;
    case _PC_NAME_MAX:
      /* NAME_MAX is without trailing \0 */
      return pc.isdir () ? PATH_MAX - strlen (get_name ()) - 2 : NAME_MAX;
    case _PC_PATH_MAX:
      /* PATH_MAX is with trailing \0 */
      return pc.isdir () ? PATH_MAX - strlen (get_name ()) - 1 : PATH_MAX;
    case _PC_PIPE_BUF:
      if (pc.isdir ()
	  || get_device () == FH_FIFO || get_device () == FH_PIPE
	  || get_device () == FH_PIPER || get_device () == FH_PIPEW)
	return PIPE_BUF;
      set_errno (EINVAL);
      break;
    case _PC_CHOWN_RESTRICTED:
      return 1;
    case _PC_NO_TRUNC:
      return 1;
    case _PC_VDISABLE:
      if (is_tty ())
	return _POSIX_VDISABLE;
      set_errno (EINVAL);
      break;
    case _PC_ASYNC_IO:
    case _PC_PRIO_IO:
      break;
    case _PC_SYNC_IO:
      return 1;
    case _PC_FILESIZEBITS:
      return FILESIZEBITS;
    case _PC_2_SYMLINKS:
      return 1;
    case _PC_SYMLINK_MAX:
      return SYMLINK_MAX;
    case _PC_POSIX_PERMISSIONS:
    case _PC_POSIX_SECURITY:
      if (get_device () == FH_FS)
	return pc.has_acls ();
      set_errno (EINVAL);
      break;
    default:
      set_errno (EINVAL);
      break;
    }
  return -1;
}

/* Overlapped I/O */

bool
fhandler_base::setup_overlapped ()
{
  OVERLAPPED *ov = get_overlapped ();
  memset (ov, 0, sizeof (*ov));
  return ov->hEvent = CreateEvent (&sec_none_nih, true, false, NULL);
}

void
fhandler_base::destroy_overlapped ()
{
  OVERLAPPED *ov = get_overlapped ();
  if (ov && ov->hEvent)
    {
      CloseHandle (ov->hEvent);
      ov->hEvent = NULL;
    }
}

int
fhandler_base::wait_overlapped (bool& res, bool writing, DWORD *bytes)
{
  if (bytes)
    *bytes = (DWORD) -1;
  DWORD err = GetLastError ();
  if (!res && err != ERROR_IO_PENDING)
    {
      if (err != ERROR_HANDLE_EOF && err != ERROR_BROKEN_PIPE)
	goto err;
      res = 1;
      if (*bytes)
	*bytes = 0;
    }
  else
    {
#ifdef DEBUGGING
      if (!get_overlapped ())
	system_printf ("get_overlapped is zero?");
      if (!get_overlapped ()->hEvent)
	system_printf ("hEvent is zero?");
#endif
      DWORD n = 1;
      HANDLE w4[2];
      w4[0] = get_overlapped ()->hEvent;
      if (&_my_tls == _main_tls)
	w4[n++] = signal_arrived;
      HANDLE h = writing ? get_output_handle () : get_handle ();
      switch (WaitForMultipleObjects (n, w4, false, INFINITE))
	{
	case WAIT_OBJECT_0:
	  if (!bytes ||
	      GetOverlappedResult (h, get_overlapped (), bytes, false))
	    res = 1;
	  else
	    {
	      err = GetLastError ();
	      goto err;
	    }
	  break;
	case WAIT_OBJECT_0 + 1:
	  CancelIo (h);
	  set_errno (EINTR);
	  res = 0;
	  break;
	default:
	  err = GetLastError ();
	  goto err;
	  break;
	}
    }
  goto out;

err:
  __seterrno_from_win_error (err);
  res = -1;
  if (err == ERROR_NO_DATA)
    raise (SIGPIPE);
out:
  ResetEvent (get_overlapped ()->hEvent);
  return res;
}

void
fhandler_base::read_overlapped (void *ptr, size_t& len)
{
#ifdef DEBUGGING
  assert (get_overlapped ());
  assert (get_overlapped ()->hEvent);
#endif
  while (1)
    {
      bool res = ReadFile (get_handle (), ptr, len, (DWORD *) &len,
			   get_overlapped ());
      int wres = wait_overlapped (res, false, (DWORD *) &len);
      if (wres || !_my_tls.call_signal_handler ())
	break;
    }
}

int
fhandler_base::write_overlapped (const void *ptr, size_t len)
{
  DWORD bytes_written;

  while (1)
    {
      bool res = WriteFile (get_output_handle (), ptr, len, &bytes_written,
			    get_overlapped ());
      int wres = wait_overlapped (res, true, &bytes_written);
      if (wres < 0)
	return -1;
      if (wres || !_my_tls.call_signal_handler ())
	break;
    }
  return bytes_written;
}
