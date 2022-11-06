/*

translates characters such as spaces into alternative chars
this is mainly for the acme text editor, since it can't handle spaces or the like

*/

#define FUSE_USE_VERSION 35

#define _GNU_SOURCE
#include <fuse.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/xattr.h>
#include <sys/file.h>

#define USED(x) ((void)(x))
#define SEP(x) ((x) == '/' || (x) == 0)

typedef struct
{
	DIR *dp;
	struct dirent *entry;
	off_t offset;
} Dir;

const char *altchars[] = {u8"␣", u8"«", u8"»", u8"´", u8"­", u8"❢"};
const char *orichars[] = {u8" ", u8"(", u8")", u8"'", u8"&", u8"!"};

char *rootdir = "/";

char *
cleanname(char *name)
{
	char *p, *q, *dotdot;
	int rooted;

	rooted = name[0] == '/';

	/*
	 * invariants:
	 *	p points at beginning of path element we're considering.
	 *	q points just past the last path element we wrote (no slash).
	 *	dotdot points just past the point where .. cannot backtrack
	 *		any further (no slash).
	 */
	p = q = dotdot = name + rooted;
	while (*p) {
		if (p[0] == '/') /* null element */
			p++;
		else if (p[0] == '.' && SEP(p[1]))
			p += 1; /* don't count the separator in case it is nul */
		else if (p[0] == '.' && p[1] == '.' && SEP(p[2])) {
			p += 2;
			if (q > dotdot) { /* can backtrack */
				while (--q > dotdot && *q != '/')
					;
			} else if (!rooted) { /* /.. is / but ./../ is .. */
				if (q != name)
					*q++ = '/';
				*q++ = '.';
				*q++ = '.';
				dotdot = q;
			}
		} else { /* real path element */
			if (q != name + rooted)
				*q++ = '/';
			while ((*q = *p) != '/' && *q != 0)
				p++, q++;
		}
	}
	if (q == name) /* empty string is really ``.'' */
		*q++ = '.';
	*q = '\0';
	return name;
}

char *
tr(const char *s, const char *from[], const char *to[], bool relroot)
{
	size_t i, j, l;
	char *buf, *p;

	if (strcmp(s, ".") == 0)
		return strdup(".");
	else if (strcmp(s, "..") == 0)
		return strdup("..");

	l = strlen(s);
	buf = p = malloc(strlen(rootdir) + l * 8 + 1);
	if (buf == NULL)
		return NULL;

	if (relroot)
		p += sprintf(buf, "%s/", rootdir);

	while (*s) {
		for (i = 0; from[i]; i++) {
			l = strlen(from[i]);
			if (strncmp(s, from[i], l) == 0) {
				for (j = 0; to[i][j]; j++) {
					*p++ = to[i][j];
				}
				s += l;
				break;
			}
		}

		if (from[i] == NULL)
			*p++ = *s++;
	}

	*p = '\0';
	cleanname(buf);
	return buf;
}

char *
alt(const char *s)
{
	return tr(s, orichars, altchars, false);
}

char *
ori(const char *s)
{
	return tr(s, altchars, orichars, true);
}

int
fsgetattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
	char *fpath;
	int res;

	res = 0;
	fpath = ori(path);
	if (fpath == NULL) {
		res = -ENOMEM;
		goto out;
	}

	res = lstat(fpath, stbuf);
	if (res < 0) {
		res = -errno;
		goto out;
	}

out:
	free(fpath);
	return res;

	USED(fi);
}

int
fsaccess(const char *path, int mask)
{
	char *fpath;
	int res;

	fpath = ori(path);
	if (fpath == NULL)
		return -ENOMEM;

	res = access(fpath, mask);
	free(fpath);
	if (res < 0)
		return -errno;
	return 0;
}

int
fsreadlink(const char *path, char *buf, size_t size)
{
	char *fpath;
	int res;

	fpath = ori(path);
	if (fpath == NULL)
		return -ENOMEM;

	res = readlink(fpath, buf, size - 1);
	free(fpath);
	if (res < 0)
		return -errno;
	buf[res] = '\0';
	return 0;
}

int
fsopendir(const char *path, struct fuse_file_info *fi)
{
	char *fpath;
	Dir *d;
	int res;

	res = 0;
	fpath = ori(path);
	d = malloc(sizeof(Dir));
	if (d == NULL || fpath == NULL) {
		res = -ENOMEM;
		goto error;
	}

	d->dp = opendir(fpath);
	if (d->dp == NULL) {
		res = -errno;
		goto error;
	}
	d->offset = 0;
	d->entry = NULL;
	fi->fh = (uint64_t)d;

out:
	free(fpath);
	return res;

error:
	free(d);
	goto out;
}

int
fsreaddir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
	Dir *d;

	d = (Dir *)(uintptr_t)fi->fh;
	if (offset != d->offset) {
		seekdir(d->dp, offset);
		d->entry = NULL;
		d->offset = offset;
	}
	while (1) {
		struct stat st;
		off_t nextoff;
		char *fpath;

		if (!d->entry) {
			d->entry = readdir(d->dp);
			if (!d->entry)
				break;
		}
		memset(&st, 0, sizeof(st));
		st.st_ino = d->entry->d_ino;
		st.st_mode = d->entry->d_type << 12;
		nextoff = telldir(d->dp);

		fpath = alt(d->entry->d_name);
		if (fpath == NULL)
			return -ENOMEM;

		if (filler(buf, fpath, &st, nextoff, 0))
			break;

		d->entry = NULL;
		d->offset = nextoff;
		free(fpath);
	}

	USED(path);
	USED(flags);
	return 0;
}

int
fsreleasedir(const char *path, struct fuse_file_info *fi)
{
	Dir *d;

	if (!fi)
		return 0;

	d = (Dir *)(uintptr_t)fi->fh;
	closedir(d->dp);
	free(d);
	USED(path);
	return 0;
}

int
fsmknod(const char *path, mode_t mode, dev_t rdev)
{
	char *fpath;
	int res;

	fpath = ori(path);
	if (fpath == NULL)
		return -ENOMEM;

	if (S_ISFIFO(mode))
		res = mkfifo(fpath, mode);
	else
		res = mknod(fpath, mode, rdev);

	free(fpath);
	if (res < 0)
		return -errno;
	return 0;
}

int
fsmkdir(const char *path, mode_t mode)
{
	char *fpath;
	int res;

	fpath = ori(path);
	if (fpath == NULL)
		return -ENOMEM;

	res = mkdir(fpath, mode);
	free(fpath);
	if (res < 0)
		return -errno;
	return 0;
}

int
fsunlink(const char *path)
{
	char *fpath;
	int res;

	fpath = ori(path);
	if (fpath == NULL)
		return -ENOMEM;

	res = unlink(fpath);
	free(fpath);
	if (res < 0)
		return -errno;
	return 0;
}

int
fsrmdir(const char *path)
{
	char *fpath;
	int res;

	fpath = ori(path);
	if (fpath == NULL)
		return -ENOMEM;

	res = rmdir(fpath);
	free(fpath);
	if (res < 0)
		return -errno;
	return 0;
}

int
fssymlink(const char *from, const char *to)
{
	char *f, *t;
	int res;

	f = ori(from);
	t = ori(to);
	if (f == NULL || t == NULL) {
		free(f);
		free(t);
		return -ENOMEM;
	}

	res = symlink(f, t);
	free(f);
	free(t);
	if (res < 0)
		return -errno;
	return 0;
}

int
fsrename(const char *from, const char *to, unsigned int flags)
{
	char *f, *t;
	int res;

	f = ori(from);
	t = ori(to);
	if (f == NULL || t == NULL) {
		free(f);
		free(t);
		return -ENOMEM;
	}

	res = rename(f, t);
	free(f);
	free(t);
	if (res < 0)
		return -errno;
	return 0;

	USED(flags);
}

int
fslink(const char *from, const char *to)
{
	char *f, *t;
	int res;

	f = ori(from);
	t = ori(to);
	if (f == NULL || t == NULL) {
		free(f);
		free(t);
		return -ENOMEM;
	}

	res = link(f, t);
	free(f);
	free(t);
	if (res < 0)
		return -errno;
	return 0;
}

int
fschmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	char *fpath;
	int res;

	fpath = ori(path);
	if (fpath == NULL)
		return -ENOMEM;

	res = chmod(fpath, mode);
	free(fpath);
	if (res < 0)
		return -errno;
	return 0;

	USED(fi);
}

int
fschown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi)
{
	char *fpath;
	int res;

	fpath = ori(path);
	if (fpath == NULL)
		return -ENOMEM;

	res = lchown(fpath, uid, gid);
	free(fpath);
	if (res < 0)
		return -errno;
	return 0;

	USED(fi);
}

int
fstruncate(const char *path, off_t size, struct fuse_file_info *fi)
{
	char *fpath;
	int res;

	fpath = ori(path);
	if (fpath == NULL)
		return -ENOMEM;

	res = truncate(fpath, size);
	free(fpath);
	if (res < 0)
		return -errno;
	return 0;

	USED(fi);
}

int
fsftruncate(const char *path, off_t size, struct fuse_file_info *fi)
{
	int res;

	res = ftruncate(fi->fh, size);
	if (res < 0)
		return -errno;
	USED(path);
	return 0;
}

int
fsutimens(const char *path, const struct timespec ts[2], struct fuse_file_info *fi)
{
	int res;
	/* don't use utime/utimes since they follow symlinks */
	res = utimensat(0, path, ts, AT_SYMLINK_NOFOLLOW);
	if (res == -1)
		return -errno;
	return 0;

	USED(fi);
}

int
fscreate(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	char *fpath;
	int fd;

	fpath = ori(path);
	if (fpath == NULL)
		return -ENOMEM;

	fd = open(fpath, fi->flags, mode);
	free(fpath);
	if (fd < 0)
		return -errno;
	fi->fh = fd;
	return 0;
}

int
fsopen(const char *path, struct fuse_file_info *fi)
{
	char *fpath;
	int fd;

	fpath = ori(path);
	if (fpath == NULL)
		return -ENOMEM;

	fd = open(fpath, fi->flags);
	free(fpath);

	if (fd == -1)
		return -errno;

	fi->fh = fd;
	return 0;
}

int
fsread(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	int res;

	res = pread(fi->fh, buf, size, offset);
	if (res == -1)
		res = -errno;
	USED(path);
	return res;
}

int
fsreadbuf(const char *path, struct fuse_bufvec **bufp, size_t size, off_t offset, struct fuse_file_info *fi)
{
	struct fuse_bufvec *src;

	src = malloc(sizeof(struct fuse_bufvec));
	if (src == NULL)
		return -ENOMEM;
	*src = FUSE_BUFVEC_INIT(size);
	src->buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
	src->buf[0].fd = fi->fh;
	src->buf[0].pos = offset;
	*bufp = src;
	USED(path);
	return 0;
}

int
fswrite(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	int res;

	res = pwrite(fi->fh, buf, size, offset);
	if (res < 0)
		res = -errno;
	USED(path);
	return res;
}

int
fswritebuf(const char *path, struct fuse_bufvec *buf, off_t offset, struct fuse_file_info *fi)
{
	struct fuse_bufvec dst = FUSE_BUFVEC_INIT(fuse_buf_size(buf));

	dst.buf[0].flags = FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK;
	dst.buf[0].fd = fi->fh;
	dst.buf[0].pos = offset;
	USED(path);
	return fuse_buf_copy(&dst, buf, FUSE_BUF_SPLICE_NONBLOCK);
}

int
fsstatfs(const char *path, struct statvfs *stbuf)
{
	char *fpath;
	int res;

	fpath = ori(path);
	if (fpath == NULL)
		return -ENOMEM;

	res = statvfs(fpath, stbuf);
	free(fpath);
	if (res < 0)
		return -errno;
	return 0;
}

int
fsflush(const char *path, struct fuse_file_info *fi)
{
	int res;

	USED(path);
	/* This is called from every close on an open file, so call the
	close on the underlying filesystem. But since flush may be
	called multiple times for an open file, this must not really
	close the file. This is important if used on a network
	filesystem like NFS which flush the data/metadata on close() */
	res = close(dup(fi->fh));
	if (res < 0)
		return -errno;

	return 0;
}

int
fsrelease(const char *path, struct fuse_file_info *fi)
{
	USED(path);
	close(fi->fh);
	return 0;
}

int
fsfsync(const char *path, int isdatasync, struct fuse_file_info *fi)
{
	int res;

	USED(path);
	USED(isdatasync);
	if (isdatasync)
		res = fdatasync(fi->fh);
	else
		res = fsync(fi->fh);
	if (res == -1)
		return -errno;
	return 0;
}

int
fsfallocate(const char *path, int mode, off_t offset, off_t length, struct fuse_file_info *fi)
{
	USED(path);
	if (mode)
		return -EOPNOTSUPP;
	return -posix_fallocate(fi->fh, offset, length);
}

int
fssetxattr(const char *path, const char *name, const char *value, size_t size, int flags)
{
	char *fpath;
	int res;

	fpath = ori(path);
	if (fpath == NULL)
		return -ENOMEM;

	res = lsetxattr(fpath, name, value, size, flags);
	free(fpath);
	if (res < 0)
		return -errno;
	return 0;
}

int
fsgetxattr(const char *path, const char *name, char *value, size_t size)
{
	char *fpath;
	int res;

	fpath = ori(path);
	if (fpath == NULL)
		return -ENOMEM;

	res = lgetxattr(fpath, name, value, size);
	free(fpath);
	if (res < 0)
		return -errno;
	return res;
}

int
fsflock(const char *path, struct fuse_file_info *fi, int op)
{
	int res;
	USED(path);
	res = flock(fi->fh, op);
	if (res < 0)
		return -errno;
	return 0;
}

struct fuse_operations ops = {
    .getattr = fsgetattr,
    .access = fsaccess,
    .readlink = fsreadlink,
    .opendir = fsopendir,
    .readdir = fsreaddir,
    .releasedir = fsreleasedir,
    .mknod = fsmknod,
    .mkdir = fsmkdir,
    .symlink = fssymlink,
    .unlink = fsunlink,
    .rmdir = fsrmdir,
    .rename = fsrename,
    .link = fslink,
    .chmod = fschmod,
    .chown = fschown,
    .truncate = fstruncate,
    .utimens = fsutimens,
    .create = fscreate,
    .open = fsopen,
    .read = fsread,
    .read_buf = fsreadbuf,
    .write = fswrite,
    .write_buf = fswritebuf,
    .statfs = fsstatfs,
    .flush = fsflush,
    .release = fsrelease,
    .fsync = fsfsync,
    .fallocate = fsfallocate,
    .setxattr = fssetxattr,
    .getxattr = fsgetxattr,
    .flock = fsflock,
};

void
usage(void)
{
	fprintf(stderr, "usage: <rootdir> <mountdir> [options]\n");
	exit(2);
}

int
main(int argc, char *argv[])
{
	struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
	int i;

	if (argc < 3)
		usage();

	for (i = 0; i < argc; i++) {
		if (i == 1)
			rootdir = argv[i];
		else
			fuse_opt_add_arg(&args, argv[i]);
	}

	return fuse_main(args.argc, args.argv, &ops, NULL);
}
