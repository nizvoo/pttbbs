// $Id$
// Memcached protocol based board data export daemon

// Copyright (c) 2010-2011, Chen-Yu Tsai <wens@csie.org>
// All rights reserved.

// TODO:
//  1. [done] add hotboard support
//  2. [done] rewrite with libevent 2.0
//  3. [done] split out independent server code
//  4. [done] add article list support
//  5. [done] add article content support
//  6. [done] encode output in UTF-8 (with UAO support)
//  7. encode article list in JSON for better structure

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/util.h>

#include <cmbbs.h>
#include <var.h>
#include <perm.h>

#include "boardd.h"

#define DEFAULT_ARTICLE_LIST 20

static int g_convert_to_utf8 = 1;

// helper function

#define BOARD_HIDDEN(bptr) (bptr->brdattr & (BRD_HIDE | BRD_TOP) || \
    (bptr->level & ~(PERM_BASIC|PERM_CHAT|PERM_PAGE|PERM_POST|PERM_LOGINOK) && \
     !(bptr->brdattr & BRD_POSTMASK)))

static void
dir_list(struct evbuffer *buf, const char *path, int offset, int length)
{
    int total, fd = -1;
    fileheader_t fhdr;

    total = get_num_records(path, sizeof(fileheader_t));

    if (total <= 0)
	return;

    while (offset < 0)
	offset += total;

    while (length < 0 || length-- > 0) {
	if (get_records_keep(path, &fhdr, sizeof(fhdr), ++offset, 1, &fd) <= 0)
	    break;

	DBCS_safe_trim(fhdr.title);

	evbuffer_add_printf(buf, "%d,%s,%s,%d,%d,%s,%s\n",
		offset, fhdr.filename, fhdr.date, fhdr.recommend,
		fhdr.filemode, fhdr.owner, fhdr.title);
    }

    if (fd >= 0)
	close(fd);
}

static void
article_list(struct evbuffer *buf, boardheader_t *bptr, int offset, int length)
{
    char path[PATH_MAX];
    setbfile(path, bptr->brdname, FN_DIR);
    dir_list(buf, path, offset, length);
}

static void
bottom_article_list(struct evbuffer *buf, boardheader_t *bptr)
{
    char path[PATH_MAX];
    setbfile(path, bptr->brdname, FN_DIR ".bottom");
    dir_list(buf, path, 0, -1);
}

static int
is_valid_article_filename(const char *filename)
{
    return !strncmp(filename, "M.", 2);
}

static int
answer_file(struct evbuffer *buf, const char *path, struct stat *st,
	    const char *ck, int cklen, int offset, int maxlen)
{
    struct stat local_st;
    int fd;

    if (st == NULL)
	st = &local_st;

    if ((fd = open(path, O_RDONLY)) < 0 || fstat(fd, st) < 0)
	goto answer_file_errout;

    if (ck && cklen) {
	char ckbuf[128];
	snprintf(ckbuf, sizeof(ckbuf), "%d-%d", (int) st->st_dev, (int) st->st_ino);
	if (strncmp(ck, ckbuf, cklen) != 0)
	    goto answer_file_errout;
    }

    if (offset < 0)
	offset += st->st_size;
    if (offset < 0)
	offset = 0;
    if (offset > st->st_size)
	goto answer_file_errout;

    if (maxlen < 0 || offset + maxlen > st->st_size)
	maxlen = st->st_size - offset;

    if (maxlen == 0) {
	close(fd);
	return 0;
    }

    if (evbuffer_add_file(buf, fd, offset, maxlen) == 0)
	return 0;

answer_file_errout:
    if (fd >= 0)
	close(fd);
    return -1;
}

static int
parse_articlepart_key(const char *key, const char **ck, int *cklen,
		      int *offset, int *maxlen, const char **filename)
{
    // <key> = <cache_key>.<offset>.<maxlen>.<filename>
    *ck = key;
    int i;
    for (i = 0; key[i]; i++) {
	if (key[i] == '.') {
	    *cklen = i;
	    break;
	}
    }
    if (key[i] != '.')
	return 0;
    key += i + 1;

    char *p;
    *offset = strtol(key, &p, 10);
    if (*p != '.')
	return 0;
    key = p + 1;

    *maxlen = strtol(key, &p, 10);
    if (*p != '.')
	return 0;

    *filename = p + 1;
    return 1;
}

static int
find_good_truncate_point_from_begin(const char *content, int size)
{
    int last_startline = 0;
    int last_charend = 0;
    int last_dbcstail = 0;
    int i;
    const char *p;
    for (i = 1, p = content; i <= size; i++, p++) {
	if (i > last_dbcstail) {
	    if (IS_DBCSLEAD(*p)) {
		last_dbcstail = i + 1;
		if (i + 1 <= size)
		    last_charend = i + 1;
	    } else
		last_charend = i;
	}
	if (*p == '\n')
	    last_startline = i;
    }
    return last_startline > 0 ? last_startline : last_charend;
}

static int
find_good_truncate_point_from_end(const char *content, int size)
{
    int i;
    const char *p;
    for (i = 1, p = content; i <= size; i++, p++)
	if (*p == '\n')
	    return i;
    return 0;
}

static int
select_article_head(const char *data, int len, int *offset, int *size, void *ctx)
{
    *offset = 0;
    *size = find_good_truncate_point_from_begin(data, len);
    return 0;
}

static int
select_article_tail(const char *data, int len, int *offset, int *size, void *ctx)
{
    *offset = find_good_truncate_point_from_end(data, len);
    *size = find_good_truncate_point_from_begin(data + *offset, len - *offset);
    return 0;
}

static int
select_article_part(const char *data, int len, int *offset, int *size, void *ctx)
{
    *offset = 0;
    *size = len;
    return 0;
}

static void
cleanup_evbuffer(const void *data, size_t datalen, void *extra)
{
    evbuffer_free((struct evbuffer *)extra);
}

static int
evbuffer_slice(struct evbuffer *buf, int offset, int size)
{
    int len = evbuffer_get_length(buf);
    if (offset + size > len)
	return -1;

    struct evbuffer *back = evbuffer_new();
    evbuffer_add_buffer(back, buf);

    if (evbuffer_add_reference(buf, evbuffer_pullup(back, len) + offset,
			       size, cleanup_evbuffer, back) == 0)
	return 0;

    evbuffer_free(back);
    return -1;
}

typedef int (*select_part_func)(const char *data, int len, int *offset, int *size, void *ctx);

static int
answer_articleselect(struct evbuffer *buf, const boardheader_t *bptr,
		     const char *rest_key, select_part_func sfunc, void *ctx)
{
    char path[PATH_MAX];
    const char *ck, *filename;
    int cklen, offset, maxlen = 0;
    struct stat st;

    if (!parse_articlepart_key(rest_key, &ck, &cklen, &offset, &maxlen, &filename))
	return -1;

    if (!is_valid_article_filename(filename))
	return -1;

    setbfile(path, bptr->brdname, filename);
    if (answer_file(buf, path, &st, ck, cklen, offset, maxlen) < 0)
	return -1;

    int sel_offset, sel_size;
    int len = evbuffer_get_length(buf);
    if (sfunc(evbuffer_pullup(buf, len), len, &sel_offset, &sel_size, ctx) != 0 ||
	evbuffer_slice(buf, sel_offset, sel_size) != 0)
	return -1;

    struct evbuffer *meta = evbuffer_new();
    evbuffer_add_printf(meta, "%d-%d,%lu,%d,%d\n",
			(int) st.st_dev, (int) st.st_ino, st.st_size, sel_offset, sel_size);
    evbuffer_prepend_buffer(buf, meta);
    evbuffer_free(meta);
    return 0;
}

static void
answer_key(struct evbuffer *buf, const char *key)
{
    int bid;
    boardheader_t *bptr;

    if (isdigit(*key)) {
	char *p;

	if ((bid = atoi(key)) == 0 || bid > MAX_BOARD)
	    return;

	if ((p = strchr(key, '.')) == NULL)
	    return;

	bptr = getbcache(bid);

	if (!bptr->brdname[0] || BOARD_HIDDEN(bptr))
	    return;

	key = p + 1;

	if (strcmp(key, "isboard") == 0)
	    evbuffer_add_printf(buf, "%d", (bptr->brdattr & BRD_GROUPBOARD) ? 0 : 1);
	else if (strcmp(key, "over18") == 0)
	    evbuffer_add_printf(buf, "%d", (bptr->brdattr & BRD_OVER18) ? 1 : 0);
	else if (strcmp(key, "hidden") == 0)
	    evbuffer_add_printf(buf, "%d", BOARD_HIDDEN(bptr) ? 1 : 0);
	else if (strcmp(key, "brdname") == 0)
	    evbuffer_add(buf, bptr->brdname, strlen(bptr->brdname));
	else if (strcmp(key, "title") == 0)
	    evbuffer_add(buf, bptr->title + 7, strlen(bptr->title) - 7);
	else if (strcmp(key, "class") == 0)
	    evbuffer_add(buf, bptr->title, 4);
	else if (strcmp(key, "BM") == 0)
	    evbuffer_add(buf, bptr->BM, strlen(bptr->BM));
	else if (strcmp(key, "parent") == 0)
	    evbuffer_add_printf(buf, "%d", bptr->parent);
	else if (strcmp(key, "count") == 0) {
	    char path[PATH_MAX];
	    setbfile(path, bptr->brdname, FN_DIR);
	    evbuffer_add_printf(buf, "%d", get_num_records(path, sizeof(fileheader_t)));
	} else if (strcmp(key, "children") == 0) {
	    if (!(bptr->brdattr & BRD_GROUPBOARD))
		return;

	    for (bid = bptr->firstchild[1]; bid > 0; bid = bptr->next[1]) {
		bptr = getbcache(bid);
		evbuffer_add_printf(buf, "%d,", bid);
	    }
	} else if (strcmp(key, "bottoms") == 0) {
	    bottom_article_list(buf, bptr);
	} else if (strncmp(key, "articles.", 9) == 0) {
	    int offset, length;

	    key += 9;

	    if (!isdigit(*key) && *key != '-')
		return;

	    offset = atoi(key);
	    p = strchr(key, '.');

	    if (!p || (length = atoi(p+1)) == 0)
		length = DEFAULT_ARTICLE_LIST;

	    article_list(buf, bptr, offset, length);
	} else if (strncmp(key, "article.", 8) == 0) {
	    if (!is_valid_article_filename(key + 8))
		return;

	    char path[PATH_MAX];
	    struct stat st;
	    int fd;

	    setbfile(path, bptr->brdname, key + 8);
	    if ((fd = open(path, O_RDONLY)) < 0)
		return;
	    if (fstat(fd, &st) < 0 ||
		st.st_size == 0 ||
		evbuffer_add_file(buf, fd, 0, st.st_size) != 0)
		close(fd);
	} else if (strncmp(key, "articlestat.", 12) == 0) {
	    if (!is_valid_article_filename(key + 12))
		return;

	    char path[PATH_MAX];
	    struct stat st;

	    setbfile(path, bptr->brdname, key + 12);
	    if (stat(path, &st) < 0)
		return;

	    evbuffer_add_printf(buf, "%d-%d,%ld", (int) st.st_dev, (int) st.st_ino, st.st_size);
	} else if (strncmp(key, "articlepart.", 12) == 0) {
	    answer_articleselect(buf, bptr, key + 12, select_article_part, NULL);
	} else if (strncmp(key, "articlehead.", 12) == 0) {
	    answer_articleselect(buf, bptr, key + 12, select_article_head, NULL);
	} else if (strncmp(key, "articletail.", 12) == 0) {
	    answer_articleselect(buf, bptr, key + 12, select_article_tail, NULL);
	} else
	    return;
    } else if (strncmp(key, "tobid.", 6) == 0) {
	bid = getbnum(key + 6);
	bptr = getbcache(bid);

	if (!bptr->brdname[0] || BOARD_HIDDEN(bptr))
	    return;

	evbuffer_add_printf(buf, "%d", bid);
#if HOTBOARDCACHE
    } else if (strncmp(key, "hotboards", 9) == 0) {
	for (bid = 0; bid < SHM->nHOTs; bid++) {
	    bptr = getbcache(SHM->HBcache[bid] + 1);
	    if (BOARD_HIDDEN(bptr))
		continue;
	    evbuffer_add_printf(buf, "%d,", SHM->HBcache[bid] + 1);
	}
#endif
    }
}

static int
move_string_end(char **buf)
{
    int n = 0;
    while (**buf != '\0') {
	(*buf)++;
	n++;
    }
    return n;
}

// Make ANSI control code
//   fg, bg, bright are the original color code (eg. 30, 42, 1)
//   provide -1 means no change
//   all -1 means reset
static void
make_ansi_ctrl(char *buf, int size, int fg, int bg, int bright)
{
    int sep = 0;
    strncpy(buf, "\033[", size);
    size -= move_string_end(&buf);
    if (bright >= 0) {
	snprintf(buf, size, "%s%d", sep ? ";" : "", bright);
	size -= move_string_end(&buf);
	sep = 1;
    }
    if (fg >= 0) {
	snprintf(buf, size, "%s%d", sep ? ";" : "", fg);
	size -= move_string_end(&buf);
	sep = 1;
    }
    if (bg >= 0) {
	snprintf(buf, size, "%s%d", sep ? ";" : "", bg);
	size -= move_string_end(&buf);
	sep = 1;
    }
    snprintf(buf, size, "m");
}

// Make extended ANSI control code
//   1 ==> 111, 0 ==> 110,
//   3x ==> 13x, 4y ==> 14y.
//   provide -1 means no change
//   all -1 means reset
static void
make_ext_ansi_ctrl(char *buf, int size, int fg, int bg, int bright)
{
    make_ansi_ctrl(buf, size,
                   fg >= 0 ? 100 + fg : fg,
                   bg >= 0 ? 100 + bg : bg,
                   bright >= 0 ? 110 + bright : bright);
}

static int
evbuffer_add_ansi_escape_code(struct evbuffer *destination, int fg, int bg, int bright)
{
    char ansicode[16];
    make_ansi_ctrl(ansicode, sizeof(ansicode), fg, bg, bright);
    return evbuffer_add_printf(destination, ansicode, strlen(ansicode));
}

static int
evbuffer_add_ext_ansi_escape_code(struct evbuffer *destination, int fg, int bg, int bright)
{
    char ansicode[24];
    make_ext_ansi_ctrl(ansicode, sizeof(ansicode), fg, bg, bright);
    return evbuffer_add_printf(destination, ansicode, strlen(ansicode));
}

// Converts given evbuffer contents to UTF-8 and returns the new buffer.
// The original buffer is freed. Returns NULL on error

struct evbuffer *
evbuffer_b2u(struct evbuffer *source)
{
    unsigned char c[16];
    int out = 0;

    if (evbuffer_get_length(source) == 0)
	return source;

    struct evbuffer *destination = evbuffer_new();

    // Peek at first byte
    while (evbuffer_copyout(source, c, 1) > 0) {
	if (isascii(c[0])) {
	    if (evbuffer_add(destination, c, 1) < 0)
		break;

	    // Remove byte from source buffer
	    evbuffer_drain(source, 1);
	    out++;
	} else {
	    // Big5
	    int todrain = 2;

	    // Handle in-character colors
	    int fg = -1, bg = -1, bright = -1;
	    int n = evbuffer_copyout(source, c, sizeof(c));
	    if (n < 2)
		break;
	    while (c[1] == '\033') {
		c[n - 1] = '\0';

		// At least have \033[m
		if (n < 4 || c[2] != '[')
		    break;

		unsigned char *p = c + 3;
		if (*p == 'm') {
		    // ANSI reset
		    fg = 7;
		    bg = 0;
		    bright = 0;
		}
		while (1) {
		    int v = (int) strtol((char *)p, (char **)&p, 10);
		    if (*p != 'm' && *p != ';')
			break;

		    if (v == 0)
			bright = 0;
		    else if (v == 1)
			bright = 1;
		    else if (v >= 30 && v <= 37)
			fg = v;
		    else if (v >= 40 && v <= 47)
			bg = v;

		    if (*p == 'm')
			break;
		    p++;
		}
		if (*p != 'm') {
		    // Skip malicious or unsupported codes
		    fg = bg = bright = -1;
		    break;
		} else {
		    evbuffer_drain(source, p - c + 1);
		    todrain = 1; // We keep a byte on buffer, so fix offset
		    n = evbuffer_copyout(source, c + 1, sizeof(c) - 1);
		    if (n < 1)
			break;
		    n++;
		}
	    }
#ifdef EXTENDED_INCHAR_ANSI
	    // Output control codes before the Big5 character
	    if (fg >= 0 || bg >= 0 || bright >= 0) {
                int dlen = evbuffer_add_ext_ansi_escape_code(destination, fg, bg, bright);
                if (dlen < 0)
                    break;
                out += dlen;
	    }
#endif

	    // n may be changed, check again
	    if (n < 2)
		break;

	    uint8_t utf8[4];
	    int len = ucs2utf(b2u_table[c[0] << 8 | c[1]], utf8);
	    utf8[len] = 0;

	    if (evbuffer_add(destination, utf8, len) < 0)
		break;

#ifndef EXTENDED_INCHAR_ANSI
            // Output in-char control codes to make state consistent
            if (fg >= 0 || bg >= 0 || bright >= 0) {
                int dlen = evbuffer_add_ansi_escape_code(destination, fg, bg, bright);
                if (dlen < 0)
                    break;
                out += dlen;
            }
#endif

	    // Remove DBCS character from source buffer
	    evbuffer_drain(source, todrain);
	    out += len;
	}
    }

    if (evbuffer_get_length(source) == 0 && out) {
	// Success
	evbuffer_free(source);
	return destination;
    }

    // Fail
    evbuffer_free(source);
    evbuffer_free(destination);
    return NULL;
}

// Command functions

int
cmd_get(struct evbuffer *output, void *ctx, int argc, char **argv)
{
    struct evbuffer *buf = evbuffer_new();

    if (*argv == NULL) {
	evbuffer_add_reference(output, "ERROR\r\n", 7, NULL, NULL);
	return 0;
    }

    do {
	answer_key(buf, *argv);
	if (evbuffer_get_length(buf) == 0)
	    continue;
	if (g_convert_to_utf8) {
	    buf = evbuffer_b2u(buf);
	    if (buf == NULL) {
		// Failed to convert
		buf = evbuffer_new();
		continue;
	    }
	}
	evbuffer_add_printf(output, "VALUE %s 0 %ld\r\n", *argv, evbuffer_get_length(buf));
	evbuffer_add_buffer(output, buf);
	evbuffer_add_printf(output, "\r\n");
    } while (*++argv);

    evbuffer_add_reference(output, "END\r\n", 5, NULL, NULL);

    evbuffer_free(buf);
    return 0;
}

int
cmd_version(struct evbuffer *output, void *ctx, int argc, char **argv)
{
    static const char msg[] = "VERSION 0.0.2\r\n";
    evbuffer_add_reference(output, msg, strlen(msg), NULL, NULL);
    return 0;
}

int
cmd_unknown(struct evbuffer *output, void *ctx, int argc, char **argv)
{
    static const char msg[] = "SERVER_ERROR Not implemented\r\n";
    evbuffer_add_reference(output, msg, strlen(msg), NULL, NULL);
    return 0;
}

int
cmd_quit(struct evbuffer *output, void *ctx, int argc, char **argv)
{
    return -1;
}

static const struct {
    const char *cmd;
    int (*func)(struct evbuffer *output, void *ctx, int argc, char **argv);
} cmdlist[] = {
    {"get", cmd_get},
    {"quit", cmd_quit},
    {"version", cmd_version},
    {NULL, cmd_unknown}
};

int
split_args(char *line, char ***argp)
{
    int argc = 0;
    char *p, **argv;

    if ((argv = calloc(MAX_ARGS + 1, sizeof(char *))) == NULL)
	return -1;

    while ((p = strsep(&line, " \t\r\n")) != NULL) {
	argv[argc++] = p;

	if (argc == MAX_ARGS)
	    break;
    }

    argv = realloc(argv, (argc + 1) * sizeof(char *));
    *argp = argv;

    return argc;
}

int
process_line(struct evbuffer *output, void *ctx, char *line)
{
    char **argv;
    int argc = split_args(line, &argv);
    int i;

    for (i = 0; cmdlist[i].cmd; i++)
	if (evutil_ascii_strcasecmp(line, cmdlist[i].cmd) == 0)
	    break;

    int result = (cmdlist[i].func)(output, ctx, argc - 1, argv + 1);

    free(argv);
    free(line);
    return result;
}

void
setup_program()
{
    setuid(BBSUID);
    setgid(BBSGID);
    chdir(BBSHOME);

    attach_SHM();
}

int main(int argc, char *argv[])
{
    int ch, run_as_daemon = 1;
    const char *iface_ip = "127.0.0.1:5150";

    while ((ch = getopt(argc, argv, "5Dl:h")) != -1)
	switch (ch) {
	    case '5':
		g_convert_to_utf8 = 0;
		break;
	    case 'D':
		run_as_daemon = 0;
		break;
	    case 'l':
		iface_ip = optarg;
		break;
	    case 'h':
	    default:
		fprintf(stderr, "Usage: %s [-5] [-D] [-l interface_ip:port]\n", argv[0]);
		exit(EXIT_FAILURE);
	}

    if (run_as_daemon)
	if (daemon(1, 1) < 0) {
	    perror("daemon");
	    exit(EXIT_FAILURE);
	}

    setup_program();

    signal(SIGPIPE, SIG_IGN);

    char *ipport = strdup(iface_ip);
    char *ip = strtok(ipport, ":");
    char *port = strtok(NULL, ":");
    start_server(ip, atoi(port));
    free(ipport);

    return 0;
}

#ifdef __linux__

int
daemon(int nochdir, int noclose)
{
    int fd;

    switch (fork()) {
	case -1:
	    return -1;
	case 0:
	    break;
	default:
	    _exit(0);
    }

    if (setsid() == -1)
	return -1;

    if (!nochdir)
	chdir("/");

    if (!noclose && (fd = open("/dev/null", O_RDWR)) >= 0) {
	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);

	if (fd > 2)
	    close(fd);
    }

    return 0;
}

#endif // __linux__
