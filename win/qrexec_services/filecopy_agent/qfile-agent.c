#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <Strsafe.h>
#include <Shlwapi.h>
#include <Shellapi.h>
#include <ioall.h>
#include <gui-fatal.h>
#include "filecopy.h"
#include "crc32.h"

enum {
	PROGRESS_FLAG_NORMAL,
	PROGRESS_FLAG_INIT,
	PROGRESS_FLAG_DONE
};

HANDLE STDIN = INVALID_HANDLE_VALUE;
HANDLE STDOUT = INVALID_HANDLE_VALUE;
HANDLE STDERR = INVALID_HANDLE_VALUE;

#ifdef DBG
#define internal_fatal gui_fatal
#else
static __inline void internal_fatal(const PWCHAR fmt, ...) {
	gui_fatal(L"Internal error");
}
#endif


unsigned long crc32_sum;
int write_all_with_crc(HANDLE hOutput, void *pBuf, int sSize)
{
	crc32_sum = Crc32_ComputeBuf(crc32_sum, pBuf, sSize);
	return write_all(hOutput, pBuf, sSize);
}

void do_notify_progress(long long total, int flag)
{
	/* TODO, Windows qrexec_client_vm runs detached from console */
#if 0
	char *du_size_env = getenv("FILECOPY_TOTAL_SIZE");
	char *progress_type_env = getenv("PROGRESS_TYPE");
	char *saved_stdout_env = getenv("SAVED_FD_1");
	if (!progress_type_env)
		return;
	if (!strcmp(progress_type_env, "console") && du_size_env) {
		char msg[256];
		snprintf(msg, sizeof(msg), "sent %lld/%lld KB\r",
			 total / 1024, strtoull(du_size_env, NULL, 0));
		write(2, msg, strlen(msg));
		if (flag == PROGRESS_FLAG_DONE)
			write(2, "\n", 1);
	}
	if (!strcmp(progress_type_env, "gui") && saved_stdout_env) {
		char msg[256];
		snprintf(msg, sizeof(msg), "%lld\n", total);
		write(strtoul(saved_stdout_env, NULL, 0), msg,
		      strlen(msg));
	}
#endif
}

void notify_progress(int size, int flag)
{
	static long long total = 0;
	static long long prev_total = 0;
	total += size;
	if (total > prev_total + PROGRESS_NOTIFY_DELTA
	    || (flag != PROGRESS_FLAG_NORMAL)) {
		do_notify_progress(total, flag);
		prev_total = total;
	}
}

PUCHAR ConvertUTF16ToUTF8(PWCHAR pwszUtf16, size_t *pcbUtf8) {
	PUCHAR pszUtf8;
	size_t cbUtf8;

	/* convert filename from UTF-16 to UTF-8 */
	/* calculate required size */
	cbUtf8 = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, pwszUtf16, -1, NULL, 0, NULL, NULL);
	if (!cbUtf8) {
		return NULL;
	}
	pszUtf8 = malloc(sizeof(PUCHAR)*cbUtf8);
	if (!pszUtf8) {
		return NULL;
	}
	if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, pwszUtf16, -1, pszUtf8, cbUtf8, NULL, NULL)) {
		free(pszUtf8);
		return NULL;
	}
	*pcbUtf8 = cbUtf8 - 1; /* without terminating NULL character */
	return pszUtf8;
}

#define UNIX_EPOCH_OFFSET 11644478640LL

void convertWindowTimeToUnix(PFILETIME srctime, unsigned int *puDstTime, unsigned int *puDstTimeNsec) {
	ULARGE_INTEGER tmp;

	tmp.LowPart = srctime->dwLowDateTime;
	tmp.HighPart = srctime->dwHighDateTime;

	*puDstTimeNsec = (unsigned int)((tmp.QuadPart % 10000000LL) * 100LL);
	*puDstTime = (unsigned int)((tmp.QuadPart / 10000000LL) - UNIX_EPOCH_OFFSET);
}


void write_headers(struct file_header *hdr, PWCHAR pwszFilename)
{
	PUCHAR pszFilenameUtf8 = NULL;
	size_t cbFilenameUtf8;

	pszFilenameUtf8 = ConvertUTF16ToUTF8(pwszFilename, &cbFilenameUtf8);
	if (!pszFilenameUtf8)
		gui_fatal(L"Cannot convert path '%ls' to UTF-8", pwszFilename);
	hdr->namelen = cbFilenameUtf8;
	if (!write_all_with_crc(STDOUT, hdr, sizeof(*hdr))
	    || !write_all_with_crc(STDOUT, pszFilenameUtf8, hdr->namelen))
		exit(1);
	free(pszFilenameUtf8);
}

int single_file_processor(PWCHAR pwszFilename, DWORD dwAttrs)
{
	struct file_header hdr;
	HANDLE hInput;
	FILETIME atime, mtime;

	if (dwAttrs & FILE_ATTRIBUTE_DIRECTORY) 
		hdr.mode = 0755 | 0040000;
	else
		hdr.mode = 0644 | 0100000;

	// FILE_FLAG_BACKUP_SEMANTICS required to access directories
	hInput = CreateFile(pwszFilename, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (hInput == INVALID_HANDLE_VALUE)
		gui_fatal(L"Cannot open file %lS", pwszFilename);

	/* FIXME: times are already retrieved by FindFirst/NextFile */
	if (!GetFileTime(hInput, NULL, &atime, &mtime)) {
		CloseHandle(hInput);
		gui_fatal(L"Cannot get file %lS time", pwszFilename);
	}
	convertWindowTimeToUnix(&atime, &hdr.atime, &hdr.atime_nsec);
	convertWindowTimeToUnix(&mtime, &hdr.mtime, &hdr.mtime_nsec);

	if ((dwAttrs & FILE_ATTRIBUTE_DIRECTORY) == 0) { /* FIXME: symlink */
		int ret;
		LARGE_INTEGER size;

		if (!GetFileSizeEx(hInput, &size)) {
			CloseHandle(hInput);
			gui_fatal(L"Cannot get file %lS size", pwszFilename);
		}
		hdr.filelen = size.QuadPart;
		write_headers(&hdr, pwszFilename);
		ret = copy_file(STDOUT, hInput, hdr.filelen, &crc32_sum);
		// if COPY_FILE_WRITE_ERROR, hopefully remote will produce a message
		if (ret != COPY_FILE_OK) {
			if (ret != COPY_FILE_WRITE_ERROR) {
				CloseHandle(hInput);
				gui_fatal(L"Copying file %lS: %s", pwszFilename,
					  copy_file_status_to_str(ret));
			} else
				// TODO #239
				exit(1);
		}
	}
	if (dwAttrs & FILE_ATTRIBUTE_DIRECTORY) {
		hdr.filelen = 0;
		write_headers(&hdr, pwszFilename);
	}
	/* TODO */
#if 0
	if (S_ISLNK(mode)) {
		char name[st->st_size + 1];
		if (readlink(pwszFilename, name, sizeof(name)) != st->st_size)
			gui_fatal(L"readlink %s", pwszFilename);
		hdr.filelen = st->st_size + 1;
		write_headers(&hdr, pwszFilename);
		if (!write_all_with_crc(1, name, st->st_size + 1))
			exit(1);
	}
#endif
	CloseHandle(hInput);
	return 0;
}

int do_fs_walk(PWCHAR pwszPath)
{
	PWCHAR pwszCurrentPath;
	size_t cchCurrentPath, cchSearchPath;
	DWORD dwAttrs;
	WIN32_FIND_DATAW ent;
	PWCHAR pwszSearchPath;
	HANDLE hSearch;

	if ((dwAttrs = GetFileAttributesW(pwszPath)) == INVALID_FILE_ATTRIBUTES)
		gui_fatal(L"Cannot get attributes of %lS", pwszPath);
	single_file_processor(pwszPath, dwAttrs);
	if (!(dwAttrs & FILE_ATTRIBUTE_DIRECTORY))
		return 0;
	cchSearchPath = wcslen(pwszPath)+3;
	pwszSearchPath = malloc(sizeof(WCHAR)*cchSearchPath);
	if (!pwszSearchPath)
		internal_fatal(L"malloc at %d", __LINE__);
	if (FAILED(StringCchPrintfW(pwszSearchPath, cchSearchPath, L"%lS\\*", pwszPath)))
		internal_fatal(L"StringCchPrintfW at %d", __LINE__);
	hSearch = FindFirstFileW(pwszSearchPath, &ent);
	if (hSearch == INVALID_HANDLE_VALUE) {
		LONG ret = GetLastError();
		if (ret == ERROR_FILE_NOT_FOUND)
			// empty directory
			return 0;
		gui_fatal(L"Cannot list directory %lS", pwszPath);
	}
	do {
		if (!wcscmp(ent.cFileName, L".") || !wcscmp(ent.cFileName, L".."))
			continue;
		cchCurrentPath = wcslen(pwszPath)+wcslen(ent.cFileName)+2;
		pwszCurrentPath = malloc(sizeof(WCHAR)*cchCurrentPath);
		if (!pwszCurrentPath)
			internal_fatal(L"malloc at %d", __LINE__);
		// use forward slash here to send it also to the other end
		if (FAILED(StringCchPrintfW(pwszCurrentPath, cchCurrentPath, L"%lS/%lS", pwszPath, ent.cFileName)))
			internal_fatal(L"StringCchPrintfW at %d", __LINE__);
		do_fs_walk(pwszCurrentPath);
		free(pwszCurrentPath);
	} while (FindNextFileW(hSearch, &ent));
	FindClose(hSearch);
	// directory metadata is resent; this makes the code simple,
	// and the atime/mtime is set correctly at the second time
	single_file_processor(pwszPath, dwAttrs);
	return 0;
}

void notify_end_and_wait_for_result()
{
	struct result_header hdr;
	struct file_header end_hdr;

	/* nofity end of transfer */
	memset(&end_hdr, 0, sizeof(end_hdr));
	end_hdr.namelen = 0;
	end_hdr.filelen = 0;
	write_all_with_crc(STDOUT, &end_hdr, sizeof(end_hdr));

	/* wait for result */
	if (!read_all(STDIN, &hdr, sizeof(hdr))) {
		// TODO #239
		exit(1);	// hopefully remote has produced error message
	}
	if (hdr.error_code != 0) {
		gui_fatal(L"Error writing files: %s",
			  strerror(hdr.error_code));
	}
	if (hdr.crc32 != crc32_sum) {
		gui_fatal(L"File transfer failed: checksum mismatch");
	}
}

PWCHAR GetAbsolutePath(PWCHAR pwszCwd, PWCHAR pwszPath)
{
	PWCHAR pwszAbsolutePath;
	size_t cchAbsolutePath;
	if (!PathIsRelative(pwszPath))
		return _wcsdup(pwszPath);
	cchAbsolutePath = wcslen(pwszCwd)+wcslen(pwszPath)+2;
	pwszAbsolutePath = malloc(sizeof(WCHAR)*cchAbsolutePath);
	if (!pwszAbsolutePath) {
		return NULL;
	}
	if (FAILED(StringCchPrintfW(pwszAbsolutePath, cchAbsolutePath, L"%lS\\%lS", pwszCwd, pwszPath))) {
		free(pwszAbsolutePath);
		return NULL;
	}
	return pwszAbsolutePath;
}

int __cdecl _tmain(ULONG argc, PTCHAR argv[])
{
	int i;
	PWCHAR *szArglist;
	int nArgs;
	PWCHAR pwszArgumentDirectory, pwszArgumentBasename;
	WCHAR wszCwd[MAX_PATH]; /* FIXME: path can be longer */

	STDERR = GetStdHandle(STD_ERROR_HANDLE);
	if (STDERR == NULL || STDERR == INVALID_HANDLE_VALUE) {
		internal_fatal(L"Failed to get STDERR handle");
		exit(1);
	}
	STDIN = GetStdHandle(STD_INPUT_HANDLE);
	if (STDIN == NULL || STDIN == INVALID_HANDLE_VALUE) {
		internal_fatal(L"Failed to get STDIN handle");
		exit(1);
	}
	STDOUT = GetStdHandle(STD_OUTPUT_HANDLE);
	if (STDOUT == NULL || STDOUT == INVALID_HANDLE_VALUE) {
		internal_fatal(L"Failed to get STDOUT handle");
		exit(1);
	}
	notify_progress(0, PROGRESS_FLAG_INIT);
	crc32_sum = 0;
	if (!GetCurrentDirectory(sizeof(wszCwd), wszCwd)) {
		internal_fatal(L"Failed to get current directory");
		exit(1);
	}
	szArglist = CommandLineToArgvW(GetCommandLineW(), &nArgs);
	for (i = 1; i < nArgs; i++) {
		pwszArgumentDirectory = GetAbsolutePath(wszCwd, szArglist[i]);
		if (!pwszArgumentDirectory) {
			gui_fatal(L"GetAbsolutePath %ls", szArglist[i]);
		}
		// absolute path has at least one character
		if (PathGetCharTypeW(pwszArgumentDirectory[wcslen(pwszArgumentDirectory)-1]) == GCT_SEPARATOR)
			pwszArgumentDirectory[wcslen(pwszArgumentDirectory)-1] = L'\0';
		pwszArgumentBasename = _wcsdup(pwszArgumentDirectory);
		PathStripPath(pwszArgumentBasename);
		PathRemoveFileSpec(pwszArgumentDirectory);

		if (!SetCurrentDirectory(pwszArgumentDirectory))
			gui_fatal(L"chdir to %s", pwszArgumentDirectory);
		do_fs_walk(pwszArgumentBasename);
		free(pwszArgumentDirectory);
		free(pwszArgumentBasename);
	}
	notify_end_and_wait_for_result();
	notify_progress(0, PROGRESS_FLAG_DONE);
	return 0;
}