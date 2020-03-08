/* See LICENSE file for copyright and license details. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "http.h"
#include "resp.h"
#include "util.h"

static int
compareent(const struct dirent **d1, const struct dirent **d2)
{
	int v;

	v = ((*d2)->d_type == DT_DIR ? 1 : -1) -
	    ((*d1)->d_type == DT_DIR ? 1 : -1);
	if (v) {
		return v;
	}

	return strcmp((*d1)->d_name, (*d2)->d_name);
}

static char *
suffix(int t)
{
	switch (t) {
	case DT_FIFO: return "|";
	case DT_DIR:  return "/";
	case DT_LNK:  return "@";
	case DT_SOCK: return "=";
	}

	return "";
}

enum status
resp_dir(int fd, char *name, struct request *r)
{
	struct dirent **e;
	size_t i;
	int dirlen, s;
	static char t[TIMESTAMP_LEN];

	/* read directory */
	if ((dirlen = scandir(name, &e, NULL, compareent)) < 0) {
		return http_send_status(fd, S_FORBIDDEN);
	}

	/* send header as late as possible */
	if (dprintf(fd,
	            "HTTP/1.1 %d %s\r\n"
	            "Date: %s\r\n"
	            "Connection: close\r\n"
		    "Content-Type: text/html; charset=utf-8\r\n"
		    "\r\n",
	            S_OK, status_str[S_OK], timestamp(time(NULL), t)) < 0) {
		s = S_REQUEST_TIMEOUT;
		goto cleanup;
	}

	if (r->method == M_GET) {
		/* listing header */
		if (dprintf(fd,
		            "<!DOCTYPE html>\n<html>\n\t<head>"
		            "<title>Index of %s</title></head>\n"
		            "\t<body>\n\t\t<a href=\"..\">..</a>",
		            name) < 0) {
			s = S_REQUEST_TIMEOUT;
			goto cleanup;
		}

		/* listing */
		for (i = 0; i < (size_t)dirlen; i++) {
			/* skip hidden files, "." and ".." */
			if (e[i]->d_name[0] == '.') {
				continue;
			}

			/* entry line */
			if (dprintf(fd, "<br />\n\t\t<a href=\"%s%s\">%s%s</a>",
			            e[i]->d_name,
			            (e[i]->d_type == DT_DIR) ? "/" : "",
			            e[i]->d_name,
			            suffix(e[i]->d_type)) < 0) {
				s = S_REQUEST_TIMEOUT;
				goto cleanup;
			}
		}

		/* listing footer */
		if (dprintf(fd, "\n\t</body>\n</html>\n") < 0) {
			s = S_REQUEST_TIMEOUT;
			goto cleanup;
		}
	}
	s = S_OK;

cleanup:
	while (dirlen--) {
		free(e[dirlen]);
	}
	free(e);

	return s;
}

enum status
resp_file(int fd, char *name, struct request *r, struct stat *st, char *mime,
          char *encoding, off_t lower, off_t upper)
{
	FILE *fp;
	enum status s;
	ssize_t bread, bwritten;
	off_t remaining;
	int range;
	static char buf[BUFSIZ], *p, t1[TIMESTAMP_LEN], t2[TIMESTAMP_LEN];

	/* open file */
	if (!(fp = fopen(name, "r"))) {
		s = http_send_status(fd, S_FORBIDDEN);
		goto cleanup;
	}

	/* seek to lower bound */
	if (fseek(fp, lower, SEEK_SET)) {
		s = http_send_status(fd, S_INTERNAL_SERVER_ERROR);
		goto cleanup;
	}

	/* send header as late as possible */
	range = r->field[REQ_RANGE][0];
	s = range ? S_PARTIAL_CONTENT : S_OK;

	if (dprintf(fd,
	            "HTTP/1.1 %d %s\r\n"
	            "Date: %s\r\n"
	            "Connection: close\r\n"
	            "Last-Modified: %s\r\n"
	            "Content-Type: %s\r\n"
				"%s"
	            "Content-Length: %zu\r\n",
	            s, status_str[s], timestamp(time(NULL), t1),
	            timestamp(st->st_mtim.tv_sec, t2), mime,
	            encoding, upper - lower + 1) < 0) {
		s = S_REQUEST_TIMEOUT;
		goto cleanup;
	}
	if (range) {
		if (dprintf(fd, "Content-Range: bytes %zd-%zd/%zu\r\n",
		            lower, upper + (upper < 0), st->st_size) < 0) {
			s = S_REQUEST_TIMEOUT;
			goto cleanup;
		}
	}
	if (dprintf(fd, "\r\n") < 0) {
		s = S_REQUEST_TIMEOUT;
		goto cleanup;
	}

	if (r->method == M_GET) {
		/* write data until upper bound is hit */
		remaining = upper - lower + 1;

		while ((bread = fread(buf, 1, MIN(sizeof(buf),
		                      (size_t)remaining), fp))) {
			if (bread < 0) {
				return S_INTERNAL_SERVER_ERROR;
			}
			remaining -= bread;
			p = buf;
			while (bread > 0) {
				bwritten = write(fd, p, bread);
				if (bwritten <= 0) {
					return S_REQUEST_TIMEOUT;
				}
				bread -= bwritten;
				p += bwritten;
			}
		}
	}
cleanup:
	if (fp) {
		fclose(fp);
	}

	return s;
}
