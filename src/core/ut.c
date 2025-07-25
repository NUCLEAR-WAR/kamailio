/*
 * various general purpose functions
 *
 * Copyright (C) 2001-2003 FhG Fokus
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */


/** Kamailio core :: various general purpose/utility functions.
 * @file ut.c
 * @ingroup core
 * Module: core
 */

#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <stdlib.h>
#include <time.h>
#include <sys/utsname.h> /* uname() */
#include <libgen.h>

#ifdef __OS_darwin
/* portable clock_gettime() */
#include <mach/clock.h>
#include <mach/mach.h>
#endif

#include "ut.h"
#include "mem/mem.h"
#include "globals.h"

/* global buffer for ut.h int2str() */
char ut_buf_int2str[INT2STR_MAX_LEN];


/* converts a username into uid:gid,
 * returns -1 on error & 0 on success */
int user2uid(int *uid, int *gid, char *user)
{
	char *tmp;
	struct passwd *pw_entry;

	if(user) {
		*uid = strtol(user, &tmp, 10);
		if((tmp == 0) || (*tmp)) {
			/* maybe it's a string */
			pw_entry = getpwnam(user);
			if(pw_entry == 0) {
				goto error;
			}
			*uid = pw_entry->pw_uid;
			if(gid)
				*gid = pw_entry->pw_gid;
		}
		return 0;
	}
error:
	return -1;
}


/* converts a group name into a gid
 * returns -1 on error, 0 on success */
int group2gid(int *gid, char *group)
{
	char *tmp;
	struct group *gr_entry;

	if(group) {
		*gid = strtol(group, &tmp, 10);
		if((tmp == 0) || (*tmp)) {
			/* maybe it's a string */
			gr_entry = getgrnam(group);
			if(gr_entry == 0) {
				goto error;
			}
			*gid = gr_entry->gr_gid;
		}
		return 0;
	}
error:
	return -1;
}


/*
 * Replacement of timegm (does not exists on all platforms)
 * Taken from
 * http://lists.samba.org/archive/samba-technical/2002-November/025737.html
 */
time_t _timegm(struct tm *t)
{
	time_t tl, tb;
	struct tm tg;

	t->tm_isdst = 0;
	tl = mktime(t);
	if(tl == -1) {
		t->tm_hour--;
		tl = mktime(t);
		if(tl == -1) {
			return -1; /* can't deal with output from strptime */
		}
		tl += 3600;
	}

	gmtime_r(&tl, &tg);
	tg.tm_isdst = 0;
	tb = mktime(&tg);
	if(tb == -1) {
		tg.tm_hour--;
		tb = mktime(&tg);
		if(tb == -1) {
			return -1; /* can't deal with output from gmtime */
		}
		tb += 3600;
	}
	return (tl - (tb - tl));
}


/* Convert time_t value that is relative to local timezone to UTC */
time_t local2utc(time_t in)
{
	struct tm tt;
	gmtime_r(&in, &tt);
	tt.tm_isdst = -1;
	return mktime(&tt);
}


/* Convert time_t value in UTC to to value relative to local time zone */
time_t utc2local(time_t in)
{
	struct tm tt;
	localtime_r(&in, &tt);
#ifdef HAVE_TIMEGM
	return timegm(&tt);
#else
	return _timegm(&tt);
#endif
}


/**
 * portable implementation for clock_gettime(CLOCK_REALTIME, ts)
 */
int ksr_clock_gettime(struct timespec *ts)
{
#ifdef __OS_darwin
	clock_serv_t cclock;
	mach_timespec_t mts;

	/* OS X does not have clock_gettime, use clock_get_time */
	host_get_clock_service(mach_host_self(), CALENDAR_CLOCK, &cclock);
	clock_get_time(cclock, &mts);
	mach_port_deallocate(mach_task_self(), cclock);
	ts->tv_sec = mts.tv_sec;
	ts->tv_nsec = mts.tv_nsec;
	return 0;
#else
	return clock_gettime(CLOCK_REALTIME, ts);
#endif
}

/*
 * Return str as zero terminated string allocated
 * using pkg_malloc
 */
char *as_asciiz(str *s)
{
	char *r;

	r = (char *)pkg_malloc(s->len + 1);
	if(!r) {
		PKG_MEM_ERROR;
		return 0;
	}
	memcpy(r, s->s, s->len);
	r[s->len] = '\0';
	return r;
}


/* return system version (major.minor.minor2) as
 *  (major<<16)|(minor)<<8|(minor2)
 * (if some of them are missing, they are set to 0)
 * if the parameters are not null they are set to the corresp. part
 */
unsigned int get_sys_version(int *major, int *minor, int *minor2)
{
	struct utsname un;
	int m1;
	int m2;
	int m3;
	char *p;

	memset(&un, 0, sizeof(un));
	m1 = m2 = m3 = 0;
	/* get sys version */
	uname(&un);
	m1 = strtol(un.release, &p, 10);
	if(*p == '.') {
		p++;
		m2 = strtol(p, &p, 10);
		if(*p == '.') {
			p++;
			m3 = strtol(p, &p, 10);
		}
	}
	if(major)
		*major = m1;
	if(minor)
		*minor = m2;
	if(minor2)
		*minor2 = m3;
	return ((m1 << 16) | (m2 << 8) | (m3));
}


/** transform a relative pathname into an absolute one.
 * @param base  - base file, used to extract the absolute path prefix.
 *                Might be NULL, in which case the path of the kamailio.cfg is
 *                used.
 * @param file  - file path to be transformed. If it's already absolute
 *                (starts with '/') is left alone. If not the result will
 *                be `dirname base`/file.
 * @return  pkg allocated asciiz string or 0 on error.
 */
char *get_abs_pathname(str *base, str *file)
{
	str ser_cfg;
	char *buf, *dir, *res;
	int len;

	if(base == NULL) {
		ser_cfg.s = cfg_file;
		ser_cfg.len = strlen(cfg_file);
		base = &ser_cfg;
	}

	if(!base->s || base->len <= 0 || base->s[0] != '/') {
		BUG("get_abs_pathname: Base file must be absolute pathname: "
			"'%.*s'\n",
				STR_FMT(base));
		return NULL;
	}

	if(!file || !file->s || file->len <= 0) {
		BUG("get_abs_pathname: Invalid 'file' parameter\n");
		return NULL;
	}

	if(file->s[0] == '/') {
		/* This is an absolute pathname, make a zero terminated
		 * copy and use it as it is */
		if((res = pkg_malloc(file->len + 1)) == NULL) {
			PKG_MEM_ERROR;
			return NULL;
		}
		memcpy(res, file->s, file->len);
		res[file->len] = 0;
	} else {
		/* This is not an absolute pathname, make it relative
		 * to the location of the base file
		 */
		/* Make a copy, function dirname may modify the string */
		if((buf = pkg_malloc(base->len + 1)) == NULL) {
			PKG_MEM_ERROR;
			return NULL;
		}
		memcpy(buf, base->s, base->len);
		buf[base->len] = 0;
		dir = dirname(buf);

		len = strlen(dir);
		if((res = pkg_malloc(len + 1 + file->len + 1)) == NULL) {
			PKG_MEM_ERROR;
			pkg_free(buf);
			return NULL;
		}
		memcpy(res, dir, len);
		res[len] = '/';
		memcpy(res + len + 1, file->s, file->len);
		res[len + 1 + file->len] = '\0';
		pkg_free(buf);
	}
	return res;
}


/**
 * @brief search for occurrence of needle in text
 * @return pointer to start of needle in text or NULL if the needle
 *	is not found
 */
char *str_search(str *text, str *needle)
{
	char *p;

	if(text == NULL || text->s == NULL || needle == NULL || needle->s == NULL
			|| text->len < needle->len)
		return NULL;

	for(p = text->s; p <= text->s + text->len - needle->len; p++) {
		if(*p == *needle->s && memcmp(p, needle->s, needle->len) == 0) {
			return p;
		}
	}

	return NULL;
}

/**
 * @brief search for occurrence of needlez starting from vstart and before vend
 * @return pointer to start of needle in text or NULL if the needle
 *	is not found
 */
char *stre_search_strz(char *vstart, char *vend, char *needlez)
{
	str text;
	str needle;

	if(vend <= vstart) {
		return NULL;
	}

	text.s = vstart;
	text.len = vend - vstart;

	needle.s = needlez;
	needle.len = strlen(needlez);

	return str_search(&text, &needle);
}

/**
 * @brief case insensitive search for occurrence of needle in text
 * @return pointer to start of needle in text or NULL if the needle
 *	is not found
 */
char *str_casesearch(str *text, str *needle)
{
	int i, j;
	for(i = 0; i <= text->len - needle->len; i++) {
		for(j = 0; j < needle->len; j++) {
			if(!((text->s[i + j] == needle->s[j])
					   || (isalpha((int)text->s[i + j])
							   && ((text->s[i + j]) ^ (needle->s[j])) == 0x20)))
				break;
		}
		if(j == needle->len)
			return text->s + i;
	}
	return NULL;
}

/**
 * portable strcasestr() - the libc version requires #define _GNU_SOURCE
 */
char *strz_casesearch_strz(char *textz, char *needlez)
{
	str text;

	text.s = textz;
	text.len = strlen(textz);

	return str_casesearch_strz(&text, needlez);
}

/**
 * case insensitive search of a charz string 'needlez' inside str 'text'
 */
char *str_casesearch_strz(str *text, char *needlez)
{
	str needle;

	needle.s = needlez;
	needle.len = strlen(needlez);

	return str_casesearch(text, &needle);
}

/**
 * @brief search for last occurrence of needle in text (reverse search)
 * @return pointer to start of needle in text or NULL if the needle
 *	is not found
 */
char *str_rsearch(str *text, str *needle)
{
	char *p;

	if(text == NULL || text->s == NULL || needle == NULL || needle->s == NULL
			|| text->len < needle->len)
		return NULL;

	for(p = text->s + text->len - needle->len; p >= text->s; p--) {
		if(*p == *needle->s && memcmp(p, needle->s, needle->len) == 0) {
			return p;
		}
	}

	return NULL;
}

/**
 * @brief case insensitive search for last occurrence of needle in text (reverse search)
 * @return pointer to start of needle in text or NULL if the needle
 *	is not found
 */
char *str_rcasesearch(str *text, str *needle)
{
	int i, j;
	for(i = text->len - needle->len; i >= 0; i--) {
		for(j = 0; j < needle->len; j++) {
			if(!((text->s[i + j] == needle->s[j])
					   || (isalpha((int)text->s[i + j])
							   && ((text->s[i + j]) ^ (needle->s[j])) == 0x20)))
				break;
		}
		if(j == needle->len)
			return text->s + i;
	}
	return NULL;
}

/*
 * ser_memmem() returns the location of the first occurrence of data
 * pattern b2 of size len2 in memory block b1 of size len1 or
 * NULL if none is found. Obtained from NetBSD.
 */
void *ser_memmem(const void *b1, const void *b2, size_t len1, size_t len2)
{
	/* Initialize search pointer */
	char *sp = (char *)b1;

	/* Initialize pattern pointer */
	char *pp = (char *)b2;

	/* Initialize end of search address space pointer */
	char *eos = sp + len1 - len2;

	/* Sanity check */
	if(!(b1 && b2 && len1 && len2))
		return NULL;

	while(sp <= eos) {
		if(*sp == *pp)
			if(memcmp(sp, pp, len2) == 0)
				return sp;

		sp++;
	}

	return NULL;
}

/*
 * ser_memrmem() returns the location of the last occurrence of data
 * pattern b2 of size len2 in memory block b1 of size len1 or
 * NULL if none is found.
 */
void *ser_memrmem(const void *b1, const void *b2, size_t len1, size_t len2)
{
	/* Initialize search pointer */
	char *sp = (char *)b1 + len1 - len2;

	/* Initialize pattern pointer */
	char *pp = (char *)b2;

	/* Initialize end of search address space pointer */
	char *eos = (char *)b1;

	/* Sanity check */
	if(!(b1 && b2 && len1 && len2))
		return NULL;

	while(sp >= eos) {
		if(*sp == *pp)
			if(memcmp(sp, pp, len2) == 0)
				return sp;

		sp--;
	}

	return NULL;
}

/**
 * decode hexa value in shex, storing in sraw
 * - on success sraw->s is pkg-allocated and has to be pkg-freed
 * - return 0 on success, -1 on failure
 */
int ksr_hex_decode_ws(str *shex, str *sraw)
{
	int i;
	char v;

	if(shex == NULL || shex->s == NULL || shex->len == 0) {
		LM_ERR("invalid body parameter\n");
		return -1;
	}

	sraw->len = shex->len / 2 + 2;
	sraw->s = pkg_malloc(sraw->len * sizeof(char));
	if(sraw->s == NULL) {
		LM_ERR("no more pkg memory\n");
		sraw->len = 0;
		return -1;
	}
	memset(sraw->s, 0, sraw->len * sizeof(char));

	sraw->len = 0;
	for(i = 0; i < shex->len; i++) {
		if(shex->s[i] == ' ' || shex->s[i] == '\t') {
			continue;
		}
		if(i + 1 == shex->len) {
			LM_ERR("invalid input hex data [%.*s] (%d/%d)\n", shex->len,
					shex->s, shex->len, i);
			goto error;
		}
		v = 0;
		if(shex->s[i] >= '0' && shex->s[i] <= '9') {
			v = (shex->s[i] - '0') << 4;
		} else if(shex->s[i] >= 'A' && shex->s[i] <= 'F') {
			v = (shex->s[i] - 'A' + 10) << 4;
		} else if(shex->s[i] >= 'a' && shex->s[i] <= 'f') {
			v = (shex->s[i] - 'a' + 10) << 4;
		} else {
			LM_ERR("invalid input hex data [%.*s] (%d/%d)\n", shex->len,
					shex->s, shex->len, i);
			goto error;
		}
		i++;
		if(shex->s[i] >= '0' && shex->s[i] <= '9') {
			v += (shex->s[i] - '0');
		} else if(shex->s[i] >= 'A' && shex->s[i] <= 'F') {
			v += (shex->s[i] - 'A' + 10);
		} else if(shex->s[i] >= 'a' && shex->s[i] <= 'f') {
			v += (shex->s[i] - 'a' + 10);
		} else {
			LM_ERR("invalid input hex data [%.*s] (%d/%d)\n", shex->len,
					shex->s, shex->len, i);
			goto error;
		}
		sraw->s[sraw->len++] = v;
	}
	if(sraw->len == 0) {
		/* only white spaces */
		LM_ERR("invalid input hex data [%.*s] (%d/%d)\n", shex->len, shex->s,
				shex->len, i);
		goto error;
	}

	return 0;

error:
	pkg_free(sraw->s);
	sraw->s = NULL;
	sraw->len = 0;
	return -1;
}
