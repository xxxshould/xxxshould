#define _GNU_SOURCE

#if __APPLE__ && __MACH__

#define _DARWIN_C_SOURCE

#include <fcntl.h>	/* For fcntl().	*/

#endif	/* Apple Macintosh */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <err.h>

#include "version.h"
#include "utils.h"

const char *adjust_unit(double *ptr_bytes)
{
	const char *units[] = { "Byte", "KB", "MB", "GB", "TB", "PB", "EB" };
	int i = 0;
	double final = *ptr_bytes;

	while (i < 7 && final >= 1024) {
		final /= 1024;
		i++;
	}
	*ptr_bytes = final;
	return units[i];
}

int is_my_file(const char *filename)
{
	const char *p = filename;

	if (!p || !isdigit(*p))
		return 0;

	/* Skip digits. */
	do {
		p++;
	} while (isdigit(*p));

	return	(p[0] == '.') && (p[1] == 'h') && (p[2] == '2') &&
		(p[3] == 'w') && (p[4] == '\0');
}

char *full_fn_from_number(const char **filename, const char *path, long num)
{
	char *str;
	assert(asprintf(&str, "%s/%li.h2w", path, num + 1) > 0);
	*filename = str + strlen(path) + 1;
	return str;
}

static long number_from_filename(const char *filename)
{
	const char *p;
	long num;

	assert(is_my_file(filename));

	p = filename;
	num = 0;
	do {
		num = num * 10 + (*p - '0');
		p++;
	} while (isdigit(*p));

	return num - 1;
}

/* Don't call this function directly, use ls_my_files() instead. */
static long *__ls_my_files(DIR *dir, long start_at, long end_at,
	int *pcount, int *pindex)
{
	struct dirent *entry;
	const char *filename;
	long number, *ret;
	int my_index;

	entry = readdir(dir);
	if (!entry) {
		ret = malloc(sizeof(long) * (*pcount + 1));
		assert(ret);
		*pindex = *pcount - 1;
		ret[*pcount] = -1;
		closedir(dir);
		return ret;
	}

	filename = entry->d_name;
	if (!is_my_file(filename))
		return __ls_my_files(dir, start_at, end_at, pcount, pindex);

	/* Cache @number because @entry may go away. */
	number = number_from_filename(filename);

	/* Ignore files before @start_at and after @end_at. */
	if (number < start_at || end_at < number)
		return __ls_my_files(dir, start_at, end_at, pcount, pindex);

	(*pcount)++;
	ret = __ls_my_files(dir, start_at, end_at, pcount, &my_index);
	ret[my_index] = number;
	*pindex = my_index - 1;
	return ret;
}

/* To be used with qsort(3). */
static int cmpintp(const void *p1, const void *p2)
{
	return *(const long *)p1 - *(const long *)p2;
}

const long *ls_my_files(const char *path, long start_at, long end_at)
{
	DIR *dir = opendir(path);
	int my_count;
	int my_index;
	long *ret;

	if (!dir)
		err(errno, "Can't open path %s", path);

	my_count = 0;
	ret = __ls_my_files(dir, start_at, end_at, &my_count, &my_index);
	assert(my_index == -1);
	qsort(ret, my_count, sizeof(*ret), cmpintp);
	return ret;
}

long arg_to_long(const struct argp_state *state, const char *arg)
{
	char *end;
	long l = strtol(arg, &end, 0);
	if (!arg)
		argp_error(state, "An integer must be provided");
	if (!*arg || *end)
		argp_error(state, "`%s' is not an integer", arg);
	return l;
}

void print_header(FILE *f, const char *name)
{
	fprintf(f,
	"F3 %s " F3_STR_VERSION "\n"
	"Copyright (C) 2010 Digirati Internet LTDA.\n"
	"This is free software; see the source for copying conditions.\n"
	"\n", name);
}

#if __APPLE__ && __MACH__

/* This function is a _rough_ approximation of fdatasync(2). */
int fdatasync(int fd)
{
	return fcntl(fd, F_FULLFSYNC);
}

/* This function is a _rough_ approximation of posix_fadvise(2). */
int posix_fadvise(int fd, off_t offset, off_t len, int advice)
{
	UNUSED(offset);
	UNUSED(len);
	switch (advice) {
	case POSIX_FADV_SEQUENTIAL:
		return fcntl(fd, F_RDAHEAD, 1);
	case POSIX_FADV_DONTNEED:
		return fcntl(fd, F_NOCACHE, 1);
	default:
		assert(0);
	}
}

#include <unistd.h> /* For usleep(). */

void msleep(double wait_ms)
{
	assert(!usleep(wait_ms * 1000));
}

#else

#include <time.h> /* For clock_gettime() and clock_nanosleep(). */

void msleep(double wait_ms)
{
	struct timespec req;
	int ret;

	assert(!clock_gettime(CLOCK_MONOTONIC, &req));

	/* Add @wait_ms to @req. */
	if (wait_ms > 1000) {
		time_t sec = wait_ms / 1000;
		wait_ms -= sec * 1000;
		assert(wait_ms > 0);
		req.tv_sec += sec;
	}
	req.tv_nsec += wait_ms * 1000000;

	/* Round @req up. */
	if (req.tv_nsec >= 1000000000) {
		ldiv_t result = ldiv(req.tv_nsec, 1000000000);
		req.tv_sec += result.quot;
		req.tv_nsec = result.rem;
	}

	do {
		ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
			&req, NULL);
	} while (ret == EINTR);

	assert(ret == 0);
}

#endif
