/*
 *	PROGRAM:	JRD Backup and Restore Program
 *	MODULE:		multivol.cpp
 *	DESCRIPTION:	
 *
 * The contents of this file are subject to the Interbase Public
 * License Version 1.0 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy
 * of the License at http://www.Inprise.com/IPL.html
 *
 * Software distributed under the License is distributed on an
 * "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, either express
 * or implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code was created by Inprise Corporation
 * and its predecessors. Portions created by Inprise Corporation are
 * Copyright (C) Inprise Corporation.
 *
 * All Rights Reserved.
 * Contributor(s): ______________________________________.
 *
 * 2002.10.29 Sean Leyne - Removed obsolete "Netware" port
 *
 */

#include "firebird.h"
#include "../jrd/ib_stdio.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#if TIME_WITH_SYS_TIME
# include <sys/time.h>
# include <time.h>
#else
# if HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif
#include "../burp/burp.h"
#ifdef WIN_NT
#include <windows.h>
#include <winnt.h>
#ifdef TEXT
#undef TEXT
#endif
#define TEXT char
#endif
#include "../burp/burp_proto.h"
#include "../burp/mvol_proto.h"
#include "../jrd/gds_proto.h"
#include "../jrd/gdsassert.h"
#include "../jrd/thd_proto.h"
#ifndef VMS
#include <fcntl.h>
#include <sys/types.h>
#else
#include <types.h>
#include <file.h>
#endif
#if (defined WIN_NT)
#include <io.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

const int open_mask	= 0666;

#ifdef VMS
const char* TERM_INPUT	= "sys$input";
const char* TERM_OUTPUT	= "sys$error";
#else
#ifdef WIN_NT
const char* TERM_INPUT	= "CONIN$";
const char* TERM_OUTPUT	= "CONOUT$";
#else
const char* TERM_INPUT	= "/dev/tty";
const char* TERM_OUTPUT	= "/dev/tty";
#endif
#endif

const int MAX_HEADER_SIZE	= 512;

static inline int get(TGBL tdgbl)
{
	return (--(tdgbl->mvol_io_cnt) >= 0 ? *(tdgbl->mvol_io_ptr)++ : 255);
}

static inline void put(TGBL tdgbl, UCHAR c)
{
	--(tdgbl->mvol_io_cnt);
	*(tdgbl->mvol_io_ptr)++ = c;
}

#ifdef DEBUG
static UCHAR debug_on = 0;		// able to turn this on in debug mode 
#endif

static void  bad_attribute(USHORT, USHORT);
static void  file_not_empty(void);
static SLONG get_numeric(void);
static int   get_text(UCHAR*, SSHORT);
static void  prompt_for_name(SCHAR*, int);
static void  put_asciz(SCHAR, const SCHAR*);
static void  put_numeric(SCHAR, int);
static bool  read_header(DESC, ULONG*, USHORT*, bool);
static bool  write_header(DESC, ULONG, bool);
static DESC	 next_volume(DESC, int, bool);


//____________________________________________________________
//
//
UINT64 MVOL_fini_read()
{
	TGBL tdgbl = GET_THREAD_DATA;

	if (strcmp(tdgbl->mvol_old_file, "stdin") != 0)
	{
		close_platf(tdgbl->file_desc);

		FIL	file;
		for (file = tdgbl->gbl_sw_backup_files; file; file = file->fil_next)
		{
			if (file->fil_fd == tdgbl->file_desc) {
				file->fil_fd = INVALID_HANDLE_VALUE;
			}
		}
	}

	tdgbl->file_desc = INVALID_HANDLE_VALUE;
	BURP_free(tdgbl->mvol_io_buffer);
	tdgbl->mvol_io_buffer = NULL;
	tdgbl->io_cnt = 0;
	tdgbl->io_ptr = NULL;
	return tdgbl->mvol_cumul_count;
}


//____________________________________________________________
//
//
UINT64 MVOL_fini_write(int* io_cnt, UCHAR** io_ptr)
{
	TGBL tdgbl = GET_THREAD_DATA;

	MVOL_write(rec_end, io_cnt, io_ptr);
	flush_platf(tdgbl->file_desc);
	if (strcmp(tdgbl->mvol_old_file, "stdout") != 0)
	{
		close_platf(tdgbl->file_desc);
		for (FIL file = tdgbl->gbl_sw_backup_files; file; file = file->fil_next)
		{
			if (file->fil_fd == tdgbl->file_desc)
				file->fil_fd = INVALID_HANDLE_VALUE;
		}
	}
	tdgbl->file_desc = INVALID_HANDLE_VALUE;
	BURP_free(tdgbl->mvol_io_header);
	tdgbl->mvol_io_header = NULL;
	tdgbl->mvol_io_buffer = NULL;
	tdgbl->io_cnt = 0;
	tdgbl->io_ptr = NULL;
	return tdgbl->mvol_cumul_count;
}


//____________________________________________________________
//
//
void MVOL_init(ULONG io_buf_size)
{
	TGBL tdgbl = GET_THREAD_DATA;

	tdgbl->mvol_io_buffer_size = io_buf_size;
}


//____________________________________________________________
//
// Read init record from backup file
//
void MVOL_init_read(const UCHAR*	database_name, // unused?
					const UCHAR*	file_name,
					USHORT*	format,
					int*	cnt,
					UCHAR**	ptr)
{
	TGBL tdgbl = GET_THREAD_DATA;

	tdgbl->mvol_volume_count = 1;
	tdgbl->mvol_empty_file = TRUE;

	if (file_name != NULL)
	{
		strncpy(tdgbl->mvol_old_file, (char*) file_name, MAX_FILE_NAME_LENGTH);
		tdgbl->mvol_old_file[MAX_FILE_NAME_LENGTH - 1] = 0;
	}
	else
	{
		tdgbl->mvol_old_file[0] = 0;
	}

	ULONG temp_buffer_size = tdgbl->mvol_io_buffer_size;
	tdgbl->mvol_actual_buffer_size = temp_buffer_size;
	tdgbl->mvol_io_buffer = BURP_alloc(temp_buffer_size);
	tdgbl->gbl_backup_start_time[0] = 0;

	read_header(tdgbl->file_desc, &temp_buffer_size, format, true);

	if (temp_buffer_size > tdgbl->mvol_actual_buffer_size)
	{
		UCHAR* new_buffer = BURP_alloc(temp_buffer_size);
		memcpy(new_buffer, tdgbl->mvol_io_buffer, tdgbl->mvol_io_buffer_size);
		BURP_free(tdgbl->mvol_io_buffer);
		tdgbl->mvol_io_ptr =
			new_buffer + (tdgbl->mvol_io_ptr - tdgbl->mvol_io_buffer);
		tdgbl->mvol_io_buffer = new_buffer;
	}

	tdgbl->mvol_actual_buffer_size = tdgbl->mvol_io_buffer_size =
		temp_buffer_size;
	*cnt = tdgbl->mvol_io_cnt;
	*ptr = tdgbl->mvol_io_ptr;
}


//____________________________________________________________
//
// Write init record to the backup file
//
void MVOL_init_write(const UCHAR*		database_name, // unused?
					 const UCHAR*		file_name,
					 int*		cnt,
					 UCHAR**	ptr)
{
	TGBL tdgbl = GET_THREAD_DATA;

	tdgbl->mvol_volume_count = 1;
	tdgbl->mvol_empty_file = TRUE;

	if (file_name != NULL)
	{
		strncpy(tdgbl->mvol_old_file, (char*)file_name, MAX_FILE_NAME_LENGTH);
		tdgbl->mvol_old_file[MAX_FILE_NAME_LENGTH - 1] = 0;
	}
	else
	{
		tdgbl->mvol_old_file[0] = 0;
	}

	tdgbl->mvol_actual_buffer_size = tdgbl->mvol_io_buffer_size;
	ULONG temp_buffer_size = tdgbl->mvol_io_buffer_size * tdgbl->gbl_sw_blk_factor;
	tdgbl->mvol_io_ptr = tdgbl->mvol_io_buffer =
		BURP_alloc(temp_buffer_size + MAX_HEADER_SIZE);
	tdgbl->mvol_io_cnt = tdgbl->mvol_actual_buffer_size;

	while (!write_header(tdgbl->file_desc, temp_buffer_size, false))
	{
		if (tdgbl->action->act_action == ACT_backup_split)
		{
			BURP_error(269, true, tdgbl->action->act_file->fil_name, 0, 0, 0, 0);
			// msg 269 can't write a header record to file %s 
		}
		tdgbl->file_desc = next_volume(tdgbl->file_desc, MODE_WRITE, false);
	}

	tdgbl->mvol_actual_buffer_size = temp_buffer_size;

	*cnt = tdgbl->mvol_io_cnt;
	*ptr = tdgbl->mvol_io_ptr;
}


#ifndef WIN_NT
//____________________________________________________________
//
// Read a buffer's worth of data. (non-WIN_NT)
//
int MVOL_read(int* cnt, UCHAR** ptr)
{
	TGBL tdgbl = GET_THREAD_DATA;

	for (;;)
	{
		tdgbl->mvol_io_cnt =
			read(	tdgbl->file_desc,
					tdgbl->mvol_io_buffer,
					tdgbl->mvol_io_buffer_size);
		tdgbl->mvol_io_ptr = tdgbl->mvol_io_buffer;

		if (tdgbl->mvol_io_cnt > 0) {
			break;
		}

		{
			if (!tdgbl->mvol_io_cnt || errno == EIO)
			{
				tdgbl->file_desc = next_volume(tdgbl->file_desc, MODE_READ, false);
				if (tdgbl->mvol_io_cnt > 0)
				{
					break;
				}
			}

			else if (!SYSCALL_INTERRUPTED(errno))
			{
				if (cnt)
				{
					BURP_error_redirect(0, 220, NULL, NULL);
				// msg 220 Unexpected I/O error while reading from backup file 
				}
				else
				{
					BURP_error_redirect(0, 50, NULL, NULL);
				// msg 50 unexpected end of file on backup file 
				}
			}
		}
	}

	tdgbl->mvol_cumul_count += tdgbl->mvol_io_cnt;
	file_not_empty();

	*ptr = tdgbl->mvol_io_ptr + 1;
	*cnt = tdgbl->mvol_io_cnt - 1;

	return *(tdgbl->mvol_io_ptr);
}


#else
//____________________________________________________________
//
// Read a buffer's worth of data. (WIN_NT)
//
int MVOL_read(int* cnt, UCHAR** ptr)
{
	TGBL tdgbl = GET_THREAD_DATA;

	for (;;)
	{
		BOOL ret = ReadFile(tdgbl->file_desc,
					   tdgbl->mvol_io_buffer,
					   tdgbl->mvol_io_buffer_size,
					   reinterpret_cast<DWORD*>(&tdgbl->mvol_io_cnt),
					   NULL);
		tdgbl->mvol_io_ptr = tdgbl->mvol_io_buffer;
		if (tdgbl->mvol_io_cnt > 0)
			break;
		else
		{
			if (!tdgbl->mvol_io_cnt)
			{
				tdgbl->file_desc = next_volume(tdgbl->file_desc, MODE_READ, false);
				if (tdgbl->mvol_io_cnt > 0)
					break;
			}
			else if (GetLastError() != ERROR_HANDLE_EOF)
			{
				if (cnt)
					BURP_error_redirect(0, 220, NULL, NULL);
				// msg 220 Unexpected I/O error while reading from backup file 
				else
					BURP_error_redirect(0, 50, NULL, NULL);
				// msg 50 unexpected end of file on backup file 
			}
		}
	}

	tdgbl->mvol_cumul_count += tdgbl->mvol_io_cnt;
	file_not_empty();

	*ptr = tdgbl->mvol_io_ptr + 1;
	*cnt = tdgbl->mvol_io_cnt - 1;

	return *(tdgbl->mvol_io_ptr);
}
#endif // !WIN_NT 


//____________________________________________________________
//
// Read a chunk of data from the IO buffer.
// Return a pointer to the first position NOT read into.
//
UCHAR* MVOL_read_block(TGBL tdgbl, UCHAR * ptr, ULONG count)
{
// To handle tape drives & Multi-volume boundaries, use the normal
// read function, instead of doing a more optimal bulk read.


	while (count)
	{
		// If buffer empty, reload it 
		if (tdgbl->io_cnt <= 0)
		{
			*ptr++ = MVOL_read(&tdgbl->io_cnt, &tdgbl->io_ptr);

			// One byte was "read" by MVOL_read 
			count--;
		}

		ULONG n = MIN(count, (ULONG) tdgbl->io_cnt);

		// Copy data from the IO buffer 

		memcpy(ptr, tdgbl->io_ptr, n);
		ptr += n;

		// Skip ahead in current buffer 

		count -= n;
		tdgbl->io_cnt -= n;
		tdgbl->io_ptr += n;

	}
	return ptr;
}


//____________________________________________________________
//
// Skip head in the IO buffer.  Often used when only
// doing partial restores.
//
void MVOL_skip_block( TGBL tdgbl, ULONG count)
{
// To handle tape drives & Multi-volume boundaries, use the normal
// read function, instead of doing a more optimal seek.


	while (count)
	{
		// If buffer empty, reload it 
		if (tdgbl->io_cnt <= 0)
		{
			MVOL_read(&tdgbl->io_cnt, &tdgbl->io_ptr);

			// One byte was "read" by MVOL_read 
			count--;
		}

		ULONG n = MIN(count, (ULONG) tdgbl->io_cnt);

		// Skip ahead in current buffer 

		count -= n;
		tdgbl->io_cnt -= n;
		tdgbl->io_ptr += n;
	}
}


#ifdef WIN_NT
//____________________________________________________________
//
// detect if it's a tape, rewind if so
// and set the buffer size
//
DESC MVOL_open(const char * name, ULONG mode, ULONG create)
{
	HANDLE handle;
	TAPE_GET_MEDIA_PARAMETERS param;
	DWORD size = sizeof(param);

	TGBL tdgbl = GET_THREAD_DATA;

	if (strnicmp(name, "\\\\.\\tape", 8))
	{
		handle = CreateFile(name, mode,
							mode == MODE_WRITE ? 0 : FILE_SHARE_READ,
							NULL, create, FILE_ATTRIBUTE_NORMAL, NULL);
	}
	else
	{
		// it's a tape device 
		// Note: we *want* to open the tape in Read-only mode or in
		// write-only mode, but it turns out that on NT SetTapePosition 
		// will fail (thereby not rewinding the tape) if the tape is 
		// opened write-only, so we will make sure that we always have 
		// read access. So much for standards!
		// Ain't Windows wonderful???
		//
		// Note: we *want* to open the tape in FILE_EXCLUSIVE_WRITE, but
		// during testing discovered that several NT tape drives do not
		// work unless we specify FILE_SHARE_WRITE as the open mode.
		// So it goes...
		//
		handle = CreateFile(name, mode | MODE_READ,
							mode == MODE_WRITE ? FILE_SHARE_WRITE : FILE_SHARE_READ,
							0, OPEN_EXISTING, 0, NULL);
		if (handle != INVALID_HANDLE_VALUE)
		{
			// emulate UNIX rewinding the tape on open:
			// This MUST be done since Windows does NOT have anything
			// like mt to allow the user to do tape management. The 
			// implication here is that we will be able to write ONLY
			// one (1) database per tape. This is bad if the user wishes to
			// backup several small databases.
			// Note: We are intentionally NOT trapping for errors during
			// rewind, since if we can not rewind, we are either a non-rewind
			// device (then it is user controlled) or we have a problem with
			// the physical media.  In the latter case I would rather wait for 
			// the write to fail so that we can loop and prompt the user for 
			// a different file/device. 
			//
			SetTapePosition(handle, TAPE_REWIND, 0, 0, 0, FALSE);
			if (GetTapeParameters(handle, GET_TAPE_MEDIA_INFORMATION, &size,
									&param) == NO_ERROR)
			{
				tdgbl->io_buffer_size = param.BlockSize;
			}
		}
	}
	return handle;
}
#endif // WIN_NT 


//____________________________________________________________
//
// Write a buffer's worth of data.
//
UCHAR MVOL_write(UCHAR c, int *io_cnt, UCHAR ** io_ptr)
{
	UCHAR *ptr;
	ULONG left, cnt;

	TGBL tdgbl = GET_THREAD_DATA;

	ULONG size_to_write = BURP_UP_TO_BLOCK(*io_ptr - tdgbl->mvol_io_buffer);

	for (ptr = tdgbl->mvol_io_buffer, left = size_to_write;
		 left > 0;
		 ptr += cnt, left -= cnt)
	{
		if (tdgbl->action->act_action == ACT_backup_split)
		{
			// Write to the current file till fil_lingth > 0, otherwise
			// switch to the next one
			if (tdgbl->action->act_file->fil_length == 0)
			{
				if (tdgbl->action->act_file->fil_next)
				{
					close_platf(tdgbl->file_desc);
					for (FIL file = tdgbl->gbl_sw_backup_files; file; file = file->fil_next)
					{
						if (file->fil_fd == tdgbl->file_desc)
							file->fil_fd = INVALID_HANDLE_VALUE;
					}
					tdgbl->action->act_file->fil_fd = INVALID_HANDLE_VALUE;
					tdgbl->action->act_file = tdgbl->action->act_file->fil_next;
					tdgbl->file_desc = tdgbl->action->act_file->fil_fd;
				}
				else
				{
					// This is a last file. Keep writing in a hope that there is
					// enough free disk space ...
					tdgbl->action->act_file->fil_length = MAX_LENGTH;
				}
			}
		}
#ifndef WIN_NT
		cnt = write(tdgbl->file_desc, ptr,
					((tdgbl->action->act_action == ACT_backup_split) &&
					 (tdgbl->action->act_file->fil_length < left) ?
					 tdgbl->action->act_file->fil_length : left));
#else
		const DWORD nBytesToWrite =
			(tdgbl->action->act_action == ACT_backup_split &&
			 (tdgbl->action->act_file->fil_length < left) ?
			 tdgbl->action->act_file->fil_length : left);
			 
		DWORD err = 0;
		// Assumes DWORD <==> ULONG
		if (!WriteFile(tdgbl->file_desc, ptr, nBytesToWrite,
					   reinterpret_cast <DWORD *>(&cnt), NULL)) 
		{
			err = GetLastError();
		}
#endif // !WIN_NT 
		tdgbl->mvol_io_buffer = tdgbl->mvol_io_data;
		if (cnt > 0)
		{
			tdgbl->mvol_cumul_count += cnt;
			file_not_empty();
			if (tdgbl->action->act_action == ACT_backup_split)
			{
				if (tdgbl->action->act_file->fil_length < left)
					tdgbl->action->act_file->fil_length = 0;
				else
					tdgbl->action->act_file->fil_length -= left;
			}
		}
		else
		{
			if (!cnt ||
#ifndef WIN_NT
				errno == ENOSPC || errno == EIO || errno == ENXIO ||
				errno == EFBIG)
#else
				err == ERROR_DISK_FULL || err == ERROR_HANDLE_DISK_FULL)
#endif // !WIN_NT 
			{
				if (tdgbl->action->act_action == ACT_backup_split)
				{
					// Close the current file and switch to the next one.
					// If there is no more specified files left then
					// issue an error and give up
					if (tdgbl->action->act_file->fil_next)
					{
						close_platf(tdgbl->file_desc);
						for (FIL file = tdgbl->gbl_sw_backup_files; file; file = file->fil_next)
						{
							if (file->fil_fd == tdgbl->file_desc)
								file->fil_fd = INVALID_HANDLE_VALUE;
						}
						
						tdgbl->action->act_file->fil_fd = INVALID_HANDLE_VALUE;
						BURP_print(272,
									tdgbl->action->act_file->fil_name,
									(void*) tdgbl->action->act_file->fil_length,
									tdgbl->action->act_file->fil_next->fil_name,
									0, 0);	// msg 272 Warning -- free disk space exhausted for file %s, the rest of the bytes (%d) will be written to file %s 
						tdgbl->action->act_file->fil_next->fil_length +=
							tdgbl->action->act_file->fil_length;
						tdgbl->action->act_file =
							tdgbl->action->act_file->fil_next;
						tdgbl->file_desc = tdgbl->action->act_file->fil_fd;
					}
					else
					{
						BURP_error(270, true, 0, 0, 0, 0, 0);
						// msg 270 free disk space exhausted 
					}
					cnt = 0;
					continue;
				}
				// Note: there is an assumption here, that if header data is being
				// written, it is really being rewritten, so at least all the
				// header data will be written

				if (left != size_to_write)
				{
					// Wrote some, move remainder up in buffer. 

					// NOTE: We should NOT use memcpy here.  We're moving overlapped
					// data and memcpy does not guanantee the order the data
					// is moved in
					memcpy(tdgbl->mvol_io_data, ptr, left);
				}
				left += tdgbl->mvol_io_data - tdgbl->mvol_io_header;
				bool full_buffer;
				if (left >=  tdgbl->mvol_io_buffer_size)
					full_buffer = true;
				else
					full_buffer = false;
				tdgbl->file_desc = next_volume(tdgbl->file_desc, MODE_WRITE, 
											   full_buffer);
				if (full_buffer)
				{
					left -= tdgbl->mvol_io_buffer_size;
					memcpy(tdgbl->mvol_io_data,
						   tdgbl->mvol_io_header + tdgbl->mvol_io_buffer_size,
						   left);
					tdgbl->mvol_cumul_count += tdgbl->mvol_io_buffer_size;
					tdgbl->mvol_io_buffer = tdgbl->mvol_io_data;
				}
				else
					tdgbl->mvol_io_buffer = tdgbl->mvol_io_header;
				break;
			}
			else if (!SYSCALL_INTERRUPTED(errno))
			{
				BURP_error_redirect(0, 221, NULL, NULL);
				// msg 221 Unexpected I/O error while writing to backup file 
			}
		}
		if (left < cnt) {	// this is impossible, but...
			cnt = left;
		}

	} // for

#ifdef DEBUG
	{
		int dbg_cnt;
		if (debug_on)
			for (dbg_cnt = 0; dbg_cnt < cnt; dbg_cnt++)
				ib_printf("%d,\n", *(ptr + dbg_cnt));
	}
#endif

// After the first block of first volume is written (using a default block size)
// change the block size to one that reflects the user's blocking factor.  By
// making the first block a standard size we will avoid restore problems.

	tdgbl->mvol_io_buffer_size = tdgbl->mvol_actual_buffer_size;

	ptr = tdgbl->mvol_io_buffer + left;
	*ptr++ = c;
	*io_ptr = ptr;
	*io_cnt = tdgbl->mvol_io_buffer_size - 1 - left;

	return c;
}


//____________________________________________________________
//
// Write a chunk of data to the IO buffer.
// Return a pointer to the first position NOT written from.
//
const UCHAR *MVOL_write_block(TGBL tdgbl, const UCHAR * ptr, ULONG count)
{
// To handle tape drives & Multi-volume boundaries, use the normal
// write function, instead of doing a more optimal bulk write.

	while (count)
	{
		// If buffer full, dump it
		if (tdgbl->io_cnt <= 0)
		{
			MVOL_write(*ptr++, &tdgbl->io_cnt, &tdgbl->io_ptr);

			// One byte was written by MVOL_write
			count--;
		}

		ULONG n = MIN(count, (ULONG) tdgbl->io_cnt);

		// Copy data to the IO buffer

		memcpy(tdgbl->io_ptr, ptr, n);
		ptr += n;

		// Skip ahead in current buffer

		count -= n;
		tdgbl->io_cnt -= n;
		tdgbl->io_ptr += n;

	}
	return ptr;
}


//____________________________________________________________
//
// We ran into an unsupported attribute.  This shouldn't happen,
// but it isn't the end of the world.
//
static void bad_attribute(USHORT attribute, USHORT type)
{
	TEXT name[128];

	TGBL tdgbl = GET_THREAD_DATA;

	gds__msg_format(0, 12, type, sizeof(name), name, 0, 0, 0, 0, 0);
	BURP_print(80, name, (void*) (ULONG) attribute, NULL, NULL, NULL);
	// msg 80  don't recognize %s attribute %ld -- continuing 
	SSHORT l = get(tdgbl);
	if (l)
	{
		do {
			get(tdgbl);
		} while (--l);
	}
}


//____________________________________________________________
//
//
static void file_not_empty(void)
{
	TGBL tdgbl = GET_THREAD_DATA;

	tdgbl->mvol_empty_file = FALSE;
}


//____________________________________________________________
//
// Get a numeric value from the input stream.
//
static SLONG get_numeric(void)
{
	SLONG value[2];

	SSHORT length = get_text((UCHAR*) value, sizeof(value));

	return gds__vax_integer((UCHAR*) value, length);
}


//____________________________________________________________
//
// Move a text attribute to a string and fill.
//
static int get_text(UCHAR* text, SSHORT length)
{
	TGBL tdgbl = GET_THREAD_DATA;

	ULONG l = get(tdgbl);
	length -= l;
	const ULONG l2 = l;

	if (length < 0)
	{
		BURP_error_redirect(0, 46, NULL, NULL);	// msg 46 string truncated 
	}

	if (l)
	{
		do {
			*text++ = get(tdgbl);
		} while (--l);
	}

	*text = 0;

	return l2;
}


//____________________________________________________________
//
// Get specification for the next volume (tape).
// Try to open it. Return file descriptor.
//
static DESC next_volume( DESC handle, int mode, bool full_buffer)
{
	TGBL tdgbl = GET_THREAD_DATA;

// We must  close the old handle before the user inserts
// another tape, or something.

#ifdef WIN_NT
	if (handle != INVALID_HANDLE_VALUE)
#else
	if (handle > -1)
#endif // WIN_NT 
	{
		close_platf(handle);
	}

	if (tdgbl->action->act_action == ACT_restore_join)
	{
		tdgbl->action->act_file->fil_fd = INVALID_HANDLE_VALUE;
		if ((tdgbl->action->act_total > tdgbl->action->act_file->fil_seq) &&
			(tdgbl->action->act_file = tdgbl->action->act_file->fil_next) &&
			(tdgbl->action->act_file->fil_fd != INVALID_HANDLE_VALUE))
		{
			return tdgbl->action->act_file->fil_fd;
		}

		BURP_error_redirect(0, 50, NULL, NULL);	// msg 50 unexpected end of file on backup file 
	}

// If we got here, we've got a live one... Up the volume number unless
// the old file was empty

	if (!tdgbl->mvol_empty_file)
		(tdgbl->mvol_volume_count)++;

	tdgbl->mvol_empty_file = TRUE;

// Loop until we have opened a file successfully 

	SCHAR new_file[MAX_FILE_NAME_LENGTH];
	DESC new_desc = INVALID_HANDLE_VALUE;
	for (;;)
	{
		// We aim to keep our descriptors clean 

		if (new_desc != INVALID_HANDLE_VALUE) {
			close_platf(new_desc);
			new_desc = INVALID_HANDLE_VALUE;
		}

		// Get file name to try 

		prompt_for_name(new_file, sizeof(new_file));

#ifdef WIN_NT
		new_desc = MVOL_open(new_file, mode, OPEN_ALWAYS);
		if (new_desc == INVALID_HANDLE_VALUE)
#else
		new_desc = open(new_file, mode, open_mask);
		if (new_desc < 0)
#endif // WIN_NT 
		{
			BURP_print(222, new_file, 0, 0, 0, 0);
			// msg 222 \n\nCould not open file name \"%s\"\n 
			continue;
		}

		// If the file is to be writable, probe it, and make sure it is... 

#ifdef WIN_NT
		if (mode == MODE_WRITE)
#else
		if ((O_WRONLY == (mode & O_WRONLY)) || (O_RDWR == (mode & O_RDWR)))
#endif // WIN_NT 
		{
			if (!write_header(new_desc, 0L, full_buffer))
			{
				BURP_print(223, new_file, 0, 0, 0, 0);
				// msg223 \n\nCould not write to file \"%s\"\n
				continue;
			}
			else
			{
				BURP_msg_put(261, (void*) (SLONG) (tdgbl->mvol_volume_count),
							 new_file, 0, 0, 0);
				// Starting with volume #vol_count, new_file 
				BURP_verbose(75, new_file, 0, 0, 0, 0);	// msg 75  creating file %s 
			}
		}
		else
		{
			// File is open for read only.  Read the header. 

			ULONG temp_buffer_size;
			USHORT format;
			if (!read_header(new_desc, &temp_buffer_size, &format, false))
			{
				BURP_print(224, new_file, 0, 0, 0, 0);
				continue;
			}
			else
			{
				BURP_msg_put(261, (void*) (SLONG) (tdgbl->mvol_volume_count),
							 new_file, 0, 0, 0);
				// Starting with volume #vol_count, new_file 
				BURP_verbose(100, new_file, 0, 0, 0, 0);	// msg 100  opened file %s 
			}
		}

		strcpy(tdgbl->mvol_old_file, new_file);
		return new_desc;
	}
}


//____________________________________________________________
//
//
static void prompt_for_name(SCHAR* name, int length)
{
	IB_FILE*	term_in = NULL;
	IB_FILE*	term_out =  NULL;
	TEXT		msg[128];

	TGBL tdgbl = GET_THREAD_DATA;

// Unless we are operating as a service, stdin can't necessarily be trusted.
// Get a location to read from.

	if (tdgbl->gbl_sw_service_gbak ||
		isatty(ib_fileno(ib_stdout)) ||
		!(term_out = ib_fopen(TERM_OUTPUT, "w")))
	{
		term_out = ib_stdout;
	}
	if (tdgbl->gbl_sw_service_gbak ||
		isatty(ib_fileno(ib_stdin)) ||
		!(term_in = ib_fopen(TERM_INPUT, "r")))
	{
		term_in = ib_stdin;
	}

// Loop until we have a file name to try 

	for (;;)
	{
		// If there was an old file name, use that prompt 

		if (strlen(tdgbl->mvol_old_file) > 0)
		{
			BURP_msg_get(225, msg, (void*) (SLONG) (tdgbl->mvol_volume_count - 1),
						 tdgbl->mvol_old_file, 0, 0, 0);
			ib_fprintf(term_out, msg);
			BURP_msg_get(226, msg, 0, 0, 0, 0, 0);
			// \tPress return to reopen that file, or type a new\n\tname 
			// followed by return to open a different file.\n
			ib_fprintf(term_out, msg);
		}
		else	// First volume 
		{
			BURP_msg_get(227, msg, 0, 0, 0, 0, 0);
			// Type a file name to open and hit return 
			ib_fprintf(term_out, msg);
		}
		BURP_msg_get(228, msg, 0, 0, 0, 0, 0);	// "  Name: " 
		ib_fprintf(term_out, msg);

		if (tdgbl->gbl_sw_service_gbak)
		{
			ib_putc('\001', term_out);
		}
		ib_fflush(term_out);
		if (ib_fgets(name, length, term_in) == NULL)
		{
			BURP_msg_get(229, msg, 0, 0, 0, 0, 0);
			// \n\nERROR: Backup incomplete\n
			ib_fprintf(term_out, msg);
			exit(FINI_ERROR);
		}

		// If the user typed just a carriage return, they
		// want the old file.  If there isn't one, reprompt

		if (name[0] == '\n')
		{
			if (strlen(tdgbl->mvol_old_file) > 0)
			{
				strcpy(name, tdgbl->mvol_old_file);
				break;
			}
			else				// reprompt 
				continue;
		}

		// OK, its a file name, strip the carriage return 

		SCHAR* name_ptr = name;
		while (*name_ptr && *name_ptr != '\n')
		{
			name_ptr++;
		}
		*name_ptr = 0;
		break;
	}

	if (term_out != ib_stdout) {
		ib_fclose(term_out);
	}
	if (term_in != ib_stdin) {
		ib_fclose(term_in);
	}
}


//____________________________________________________________
//
// Write an attribute starting with a null terminated string.
//
static void put_asciz( SCHAR attribute, const TEXT* string)
{
	TGBL tdgbl = GET_THREAD_DATA;

	SSHORT l = 0;
	for (const TEXT *p = string; *p; p++)
	{
		l++;
	}
	fb_assert(l <= MAX_UCHAR);

	put(tdgbl, attribute);
	put(tdgbl, l);
	if (l)
	{
		do {
			put(tdgbl, *string++);
		} while (--l);
	}
}


//____________________________________________________________
//
// Write a numeric value as an attribute.  The number is represented
// low byte first, high byte last, as in VAX.
//
static void put_numeric( SCHAR attribute, int value)
{
	TGBL tdgbl = GET_THREAD_DATA;

	ULONG vax_value = gds__vax_integer((UCHAR *) & value, sizeof(value));
	UCHAR* p = (UCHAR *) &vax_value;

	put(tdgbl, attribute);
	put(tdgbl, sizeof(value));

	for (USHORT i = 0; i < sizeof(value); i++) {
		put(tdgbl, *p++);
	}
}


//____________________________________________________________
//
// Functional description
//
static bool read_header(DESC	handle,
						ULONG*	buffer_size,
						USHORT*	format,
						bool	init_flag)
{
	SSHORT l;
	ULONG temp_buffer_size;
	TEXT buffer[256], *p, msg[128];

	TGBL tdgbl = GET_THREAD_DATA;

// Headers are a version number, and a volume number 

// CVC: Nobody does an explicit check for the read operation, assuming
// that GET_ATTRIBUTE() != rec_burp will provide an implicit test.
#ifndef WIN_NT
	tdgbl->mvol_io_cnt = read(handle, tdgbl->mvol_io_buffer, 
							  tdgbl->mvol_actual_buffer_size);
#else
	ReadFile(handle, tdgbl->mvol_io_buffer, tdgbl->mvol_actual_buffer_size,
			 reinterpret_cast<DWORD*>(&tdgbl->mvol_io_cnt), NULL);
#endif
	tdgbl->mvol_io_ptr = tdgbl->mvol_io_buffer;

	int attribute = get(tdgbl);
	if (attribute != rec_burp)
		BURP_error_redirect(0, 45, NULL, NULL);
		// msg 45 expected backup description record 

	int temp;
	for (attribute = get(tdgbl); attribute != att_end; attribute = get(tdgbl))
	{
		switch (attribute)
		{
		case att_backup_blksize:
			temp_buffer_size = get_numeric();
			if (init_flag)
				*buffer_size = temp_buffer_size;
			break;

		case att_backup_compress:
			temp = get_numeric();
			if (init_flag)
				tdgbl->gbl_sw_compress = temp;
			break;

		case att_backup_date:
			l = get(tdgbl);
			if (init_flag)
				p = tdgbl->gbl_backup_start_time;
			else
				p = buffer;
			if (l)
				do {
					*p++ = get(tdgbl);
				} while (--l);
			*p = 0;
			if (!init_flag && strcmp(buffer, tdgbl->gbl_backup_start_time))
			{
				BURP_msg_get(230, msg,
							 tdgbl->gbl_backup_start_time, buffer, 0, 0, 0);
				// Expected backup start time %s, found %s\n
				ib_printf(msg);
				return false;
			}
			break;

		case att_backup_file:
			l = get(tdgbl);
			if (init_flag)
			{
				p = tdgbl->mvol_db_name_buffer;
			}
			else
			{
				p = buffer;
			}
			if (l)
			{
				do {
					*p++ = get(tdgbl);
				} while (--l);
			}
			*p = 0;
			if (!init_flag && strcmp(buffer, tdgbl->gbl_database_file_name))
			{
				BURP_msg_get(231, msg,
							 tdgbl->gbl_database_file_name, buffer, 0, 0, 0);
				// Expected backup database %s, found %s\n
				ib_printf(msg);
				return false;
			}
			if (init_flag)
			{
				tdgbl->gbl_database_file_name = tdgbl->mvol_db_name_buffer;
			}
			break;

		case att_backup_format:
			temp = get_numeric();
			if (init_flag)
				*format = temp;
			break;

		case att_backup_transportable:
			temp = get_numeric();
			if (init_flag)
			{
				tdgbl->gbl_sw_transportable = temp;
			}
			break;

		case att_backup_volume:
			temp = get_numeric();
			if (temp != tdgbl->mvol_volume_count)
			{
				BURP_msg_get(232, msg,
							 (void*) (SLONG) (tdgbl->mvol_volume_count),
							 (void*) (SLONG) temp, 0, 0, 0);
				// Expected volume number %d, found volume %d\n
				ib_printf(msg);
				return false;
			}
			break;

		default:
			// TMN: Here we should really have the following assert 
			// assert(attribute <= MAX_USHORT); 
			bad_attribute((USHORT) attribute, 59);	// msg 59 backup 
		}
	}

	return true;
}


//____________________________________________________________
//
//
static bool write_header(DESC   handle,
						 ULONG  backup_buffer_size,
						 bool full_buffer)
{
	TGBL tdgbl = GET_THREAD_DATA;

	if (backup_buffer_size)
	{
		tdgbl->mvol_io_header = tdgbl->mvol_io_buffer;

		put(tdgbl, rec_burp);
		put_numeric(att_backup_format, ATT_BACKUP_FORMAT);

		if (tdgbl->gbl_sw_compress)
			put_numeric(att_backup_compress, 1);

		if (tdgbl->gbl_sw_transportable)
			put_numeric(att_backup_transportable, 1);

		put_numeric(att_backup_blksize, backup_buffer_size);

		tdgbl->mvol_io_volume = tdgbl->mvol_io_ptr + 2;
		put_numeric(att_backup_volume, tdgbl->mvol_volume_count);

		put_asciz(att_backup_file, tdgbl->gbl_database_file_name);
		put_asciz(att_backup_date, tdgbl->gbl_backup_start_time);
		put(tdgbl, att_end);

		tdgbl->mvol_io_data = tdgbl->mvol_io_ptr;
	}
	else
	{
		ULONG vax_value = gds__vax_integer((UCHAR*) &(tdgbl->mvol_volume_count),
											sizeof(tdgbl->mvol_volume_count));
		const UCHAR *p = (UCHAR *) &vax_value;
		UCHAR *q = tdgbl->mvol_io_volume;
		// CVC: Warning, do we want sizeof(int) or sizeof(some_abstract_FB_type)???
		// It seems to me we want sizeof(ULONG) for safety.
		for (USHORT i = 0; i < sizeof(int); i++)
		{
			*q++ = *p++;
		}
	}

	if (full_buffer)
	{
#ifdef WIN_NT
		DWORD bytes_written = 0;
		DWORD err = WriteFile(handle, tdgbl->mvol_io_header,
							  tdgbl->mvol_io_buffer_size, &bytes_written, NULL);
#else
		ULONG bytes_written = write(handle, tdgbl->mvol_io_header,
							  tdgbl->mvol_io_buffer_size);
#endif // WIN_NT 

		if (bytes_written != tdgbl->mvol_io_buffer_size)
		{
			return false;
		}

		if (tdgbl->action->act_action == ACT_backup_split)
		{
			if (tdgbl->action->act_file->fil_length > bytes_written)
			{
				tdgbl->action->act_file->fil_length -= bytes_written;
			}
			else
			{
				tdgbl->action->act_file->fil_length = 0;
			}
		}
		tdgbl->mvol_empty_file = FALSE;
	}

	return true;
}


//____________________________________________________________
//
// Write a header record for split operation
//
bool MVOL_split_hdr_write(void)
{
	TEXT buffer[HDR_SPLIT_SIZE + 1];

	TGBL tdgbl = GET_THREAD_DATA;

	assert(tdgbl->action->act_action == ACT_backup_split);
	assert(tdgbl->action->act_file->fil_fd != INVALID_HANDLE_VALUE);

	if (tdgbl->action->act_file->fil_length < HDR_SPLIT_SIZE) {
		return false;
	}

	time_t seconds = time(NULL);

	sprintf(buffer, "%s%.24s      , file No. %4d of %4d, %-27.27s",
			HDR_SPLIT_TAG, ctime(&seconds), tdgbl->action->act_file->fil_seq,
			tdgbl->action->act_total, tdgbl->action->act_file->fil_name);

#ifdef WIN_NT
	DWORD bytes_written = 0;
	WriteFile(tdgbl->action->act_file->fil_fd, buffer, HDR_SPLIT_SIZE,
							&bytes_written, NULL);
#else
	ULONG bytes_written =
		write(tdgbl->action->act_file->fil_fd, buffer, HDR_SPLIT_SIZE);
#endif // WIN_NT 

	if (bytes_written != HDR_SPLIT_SIZE) {
		return false;
	}

	tdgbl->action->act_file->fil_length -= bytes_written;
	return true;
}


//____________________________________________________________
//
// Read a header record for join operation
//
bool MVOL_split_hdr_read(void)
{
	TGBL tdgbl = GET_THREAD_DATA;

	assert(tdgbl->action->act_file->fil_fd != INVALID_HANDLE_VALUE);

	if (tdgbl->action && tdgbl->action->act_file &&
		(tdgbl->action->act_file->fil_fd != INVALID_HANDLE_VALUE))
	{
		TEXT buffer[HDR_SPLIT_SIZE];
		int cnt;

#ifdef WIN_NT
		ReadFile(tdgbl->action->act_file->fil_fd, buffer, HDR_SPLIT_SIZE,
					reinterpret_cast<DWORD*>(&cnt), NULL);
#else
		cnt = read(tdgbl->action->act_file->fil_fd, buffer, HDR_SPLIT_SIZE);
#endif
		if ((cnt >= 0) && ((ULONG) cnt == HDR_SPLIT_SIZE) &&
			((strncmp(buffer, HDR_SPLIT_TAG, (sizeof(HDR_SPLIT_TAG) - 1)) == 0) ||
			(strncmp(buffer, HDR_SPLIT_TAG5, (sizeof(HDR_SPLIT_TAG) - 1)) == 0)))
		{
			HDR_SPLIT hdr = (HDR_SPLIT) buffer;
			if ((tdgbl->action->act_file->fil_seq = atoi(hdr->hdr_split_sequence)) > 0 &&
				(tdgbl->action->act_total = atoi(hdr->hdr_split_total)) > 0 &&
				(tdgbl->action->act_file->fil_seq <= tdgbl->action->act_total))
			{
				return true;
			}
		}
	}

	return false;
}

