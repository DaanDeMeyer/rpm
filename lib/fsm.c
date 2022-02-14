/** \ingroup payload
 * \file lib/fsm.c
 * File state machine to handle a payload from a package.
 */

#include "system.h"

#include <inttypes.h>
#include <utime.h>
#include <errno.h>
#include <fcntl.h>
#if WITH_CAP
#include <sys/capability.h>
#endif

#include <rpm/rpmte.h>
#include <rpm/rpmts.h>
#include <rpm/rpmlog.h>
#include <rpm/rpmmacro.h>

#include "rpmio/rpmio_internal.h"	/* fdInit/FiniDigest */
#include "lib/fsm.h"
#include "lib/rpmte_internal.h"	/* XXX rpmfs */
#include "lib/rpmfi_internal.h" /* rpmfiSetOnChdir */
#include "lib/rpmplugins.h"	/* rpm plugins hooks */
#include "lib/rpmug.h"

#include "debug.h"

#define	_FSM_DEBUG	0
int _fsm_debug = _FSM_DEBUG;

/* XXX Failure to remove is not (yet) cause for failure. */
static int strict_erasures = 0;

#define	SUFFIX_RPMORIG	".rpmorig"
#define	SUFFIX_RPMSAVE	".rpmsave"
#define	SUFFIX_RPMNEW	".rpmnew"

/* Default directory and file permissions if not mapped */
#define _dirPerms 0755
#define _filePerms 0644

enum filestage_e {
    FILE_COMMIT = -1,
    FILE_NONE   = 0,
    FILE_PRE    = 1,
    FILE_UNPACK = 2,
    FILE_PREP   = 3,
    FILE_POST   = 4,
};

struct filedata_s {
    int stage;
    int setmeta;
    int skip;
    rpmFileAction action;
    const char *suffix;
    char *fpath;
    struct stat sb;
};

/* 
 * XXX Forward declarations for previously exported functions to avoid moving 
 * things around needlessly 
 */ 
static const char * fileActionString(rpmFileAction a);

/** \ingroup payload
 * Build path to file from file info, optionally ornamented with suffix.
 * @param fi		file info iterator
 * @param suffix	suffix to use (NULL disables)
 * @param[out]		path to file (malloced)
 */
static char * fsmFsPath(rpmfi fi, const char * suffix)
{
    return rstrscat(NULL, rpmfiDN(fi), rpmfiBN(fi), suffix ? suffix : "", NULL);
}

static int fsmLink(int odirfd, const char *opath, int dirfd, const char *path)
{
    int rc = linkat(odirfd, opath, dirfd, path, 0);

    if (_fsm_debug) {
	rpmlog(RPMLOG_DEBUG, " %8s (%d %s, %d %s) %s\n", __func__,
	       odirfd, opath, dirfd, path, (rc < 0 ? strerror(errno) : ""));
    }

    if (rc < 0)
	rc = RPMERR_LINK_FAILED;
    return rc;
}

static int fsmSetFCaps(const char *path, const char *captxt)
{
    int rc = 0;
#if WITH_CAP
    if (captxt && *captxt != '\0') {
	cap_t fcaps = cap_from_text(captxt);
	if (fcaps == NULL || cap_set_file(path, fcaps) != 0) {
	    rc = RPMERR_SETCAP_FAILED;
	}
	if (_fsm_debug) {
	    rpmlog(RPMLOG_DEBUG, " %8s (%s, %s) %s\n", __func__,
		   path, captxt, (rc < 0 ? strerror(errno) : ""));
	}
	cap_free(fcaps);
    } 
#endif
    return rc;
}

static int fsmClose(FD_t *wfdp)
{
    int rc = 0;
    if (wfdp && *wfdp) {
	int myerrno = errno;
	static int oneshot = 0;
	static int flush_io = 0;
	int fdno = Fileno(*wfdp);

	if (!oneshot) {
	    flush_io = (rpmExpandNumeric("%{?_flush_io}") > 0);
	    oneshot = 1;
	}
	if (flush_io) {
	    fsync(fdno);
	}
	if (Fclose(*wfdp))
	    rc = RPMERR_CLOSE_FAILED;

	if (_fsm_debug) {
	    rpmlog(RPMLOG_DEBUG, " %8s ([%d]) %s\n", __func__,
		   fdno, (rc < 0 ? strerror(errno) : ""));
	}
	*wfdp = NULL;
	errno = myerrno;
    }
    return rc;
}

static int fsmOpen(FD_t *wfdp, int dirfd, const char *dest)
{
    int rc = 0;
    /* Create the file with 0200 permissions (write by owner). */
    int fd = openat(dirfd, dest, O_WRONLY|O_EXCL|O_CREAT, 0200);

    if (fd >= 0) {
	*wfdp = fdDup(fd);
	close(fd);
    }

    if (fd < 0 || Ferror(*wfdp))
	rc = RPMERR_OPEN_FAILED;

    if (_fsm_debug) {
	rpmlog(RPMLOG_DEBUG, " %8s (%s [%d]) %s\n", __func__,
	       dest, Fileno(*wfdp), (rc < 0 ? strerror(errno) : ""));
    }

    if (rc)
	fsmClose(wfdp);

    return rc;
}

static int fsmUnpack(rpmfi fi, FD_t fd, rpmpsm psm, int nodigest)
{
    int rc = rpmfiArchiveReadToFilePsm(fi, fd, nodigest, psm);
    if (_fsm_debug) {
	rpmlog(RPMLOG_DEBUG, " %8s (%s %" PRIu64 " bytes [%d]) %s\n", __func__,
	       rpmfiFN(fi), rpmfiFSize(fi), Fileno(fd),
	       (rc < 0 ? strerror(errno) : ""));
    }
    return rc;
}

static int fsmMkfile(int dirfd, rpmfi fi, struct filedata_s *fp, rpmfiles files,
		     rpmpsm psm, int nodigest,
		     struct filedata_s ** firstlink, FD_t *firstlinkfile)
{
    int rc = 0;
    FD_t fd = NULL;

    if (*firstlink == NULL) {
	/* First encounter, open file for writing */
	rc = fsmOpen(&fd, dirfd, fp->fpath);
	/* If it's a part of a hardlinked set, the content may come later */
	if (fp->sb.st_nlink > 1) {
	    *firstlink = fp;
	    *firstlinkfile = fd;
	}
    } else {
	/* Create hard links for others and avoid redundant metadata setting */
	if (*firstlink != fp) {
	    rc = fsmLink(dirfd, (*firstlink)->fpath, dirfd, fp->fpath);
	}
	fd = *firstlinkfile;
    }

    /* If the file has content, unpack it */
    if (rpmfiArchiveHasContent(fi)) {
	if (!rc)
	    rc = fsmUnpack(fi, fd, psm, nodigest);
	/* Last file of hardlink set, ensure metadata gets set */
	if (*firstlink) {
	    fp->setmeta = 1;
	    *firstlink = NULL;
	    *firstlinkfile = NULL;
	}
    }

    if (fd != *firstlinkfile)
	fsmClose(&fd);

    return rc;
}

static int fsmReadLink(const char *path,
		       char *buf, size_t bufsize, size_t *linklen)
{
    ssize_t llen = readlink(path, buf, bufsize - 1);
    int rc = RPMERR_READLINK_FAILED;

    if (_fsm_debug) {
        rpmlog(RPMLOG_DEBUG, " %8s (%s, buf, %d) %s\n",
	       __func__,
               path, (int)(bufsize -1), (llen < 0 ? strerror(errno) : ""));
    }

    if (llen >= 0) {
	buf[llen] = '\0';
	rc = 0;
	*linklen = llen;
    }
    return rc;
}

static int fsmStat(const char *path, int dolstat, struct stat *sb)
{
    int rc;
    if (dolstat){
	rc = lstat(path, sb);
    } else {
        rc = stat(path, sb);
    }
    if (_fsm_debug && rc && errno != ENOENT)
        rpmlog(RPMLOG_DEBUG, " %8s (%s, ost) %s\n",
               __func__,
               path, (rc < 0 ? strerror(errno) : ""));
    if (rc < 0) {
        rc = (errno == ENOENT ? RPMERR_ENOENT : RPMERR_LSTAT_FAILED);
	/* Ensure consistent struct content on failure */
        memset(sb, 0, sizeof(*sb));
    }
    return rc;
}

static int fsmRmdir(const char *path)
{
    int rc = rmdir(path);
    if (_fsm_debug)
	rpmlog(RPMLOG_DEBUG, " %8s (%s) %s\n", __func__,
	       path, (rc < 0 ? strerror(errno) : ""));
    if (rc < 0)
	switch (errno) {
	case ENOENT:        rc = RPMERR_ENOENT;    break;
	case ENOTEMPTY:     rc = RPMERR_ENOTEMPTY; break;
	default:            rc = RPMERR_RMDIR_FAILED; break;
	}
    return rc;
}

static int fsmMkdir(int dirfd, const char *path, mode_t mode)
{
    int rc = mkdirat(dirfd, path, (mode & 07777));
    if (_fsm_debug)
	rpmlog(RPMLOG_DEBUG, " %8s (%d %s, 0%04o) %s\n", __func__,
	       dirfd, path, (unsigned)(mode & 07777),
	       (rc < 0 ? strerror(errno) : ""));
    if (rc < 0)	rc = RPMERR_MKDIR_FAILED;
    return rc;
}

static int fsmOpenat(int dirfd, const char *path, int flags)
{
    struct stat lsb, sb;
    int sflags = flags | O_NOFOLLOW;
    int fd = openat(dirfd, path, sflags);

    /*
     * Only ever follow symlinks by root or target owner. Since we can't
     * open the symlink itself, the order matters: we stat the link *after*
     * opening the target, and if the link ownership changed between the calls
     * it could've only been the link owner or root.
     */
    if (fd < 0 && errno == ELOOP && flags != sflags) {
	int ffd = openat(dirfd, path, flags);
	if (ffd >= 0 && fstatat(dirfd, path, &lsb, AT_SYMLINK_NOFOLLOW) == 0) {
	    if (fstat(ffd, &sb) == 0) {
		if (lsb.st_uid == 0 || lsb.st_uid == sb.st_uid) {
		    fd = ffd;
		} else {
		    close(ffd);
		}
	    }
	}
    }
    return fd;
}

static int fsmDoMkDir(rpmPlugins plugins, int dirfd, const char *dn,
			int owned, mode_t mode)
{
    int rc;
    rpmFsmOp op = (FA_CREATE);
    if (!owned)
	op |= FAF_UNOWNED;

    /* Run fsm file pre hook for all plugins */
    rc = rpmpluginsCallFsmFilePre(plugins, NULL, dn, mode, op);

    if (!rc)
	rc = fsmMkdir(dirfd, dn, mode);

    if (!rc) {
	rc = rpmpluginsCallFsmFilePrepare(plugins, NULL, dn, dn, mode, op);
    }

    /* Run fsm file post hook for all plugins */
    rpmpluginsCallFsmFilePost(plugins, NULL, dn, mode, op, rc);

    if (!rc) {
	rpmlog(RPMLOG_DEBUG,
		"%s directory created with perms %04o\n",
		dn, (unsigned)(mode & 07777));
    }

    return rc;
}

static int ensureDir(rpmPlugins plugins, const char *p, int owned, int create,
		    int *dirfdp)
{
    char *path = xstrdup(p);
    char *dp = path;
    char *sp = NULL, *bn;
    int oflags = O_RDONLY;
    int rc = 0;

    if (*dirfdp >= 0)
	return rc;

    int dirfd = fsmOpenat(-1, "/", oflags);
    int fd = dirfd; /* special case of "/" */

    while ((bn = strtok_r(dp, "/", &sp)) != NULL) {
	struct stat sb;
	fd = fsmOpenat(dirfd, bn, oflags);

	if (fd < 0 && errno == ENOENT && create) {
	    mode_t mode = S_IFDIR | (_dirPerms & 07777);
	    rc = fsmDoMkDir(plugins, dirfd, bn, owned, mode);
	    if (!rc)
		fd = fsmOpenat(dirfd, bn, oflags|O_NOFOLLOW);
	}

	if (fd >= 0 && fstat(fd, &sb) == 0 && !S_ISDIR(sb.st_mode)) {
	    rc = RPMERR_ENOTDIR;
	    break;
	}

	close(dirfd);
	if (fd >= 0) {
	    dirfd = fd;
	} else {
	    rpmlog(RPMLOG_ERR, _("failed to open dir %s of %s: %s\n"),
				bn, p, strerror(errno));
	    rc = RPMERR_OPEN_FAILED;
	    break;
	}

	dp = NULL;
    }

    if (rc) {
	close(fd);
	close(dirfd);
	dirfd = -1;
    } else {
	rc = 0;
    }
    *dirfdp = dirfd;

    free(path);
    return rc;
}

static int fsmMkfifo(int dirfd, const char *path, mode_t mode)
{
    int rc = mkfifoat(dirfd, path, (mode & 07777));

    if (_fsm_debug) {
	rpmlog(RPMLOG_DEBUG, " %8s (%d %s, 0%04o) %s\n",
	       __func__, dirfd, path, (unsigned)(mode & 07777),
	       (rc < 0 ? strerror(errno) : ""));
    }

    if (rc < 0)
	rc = RPMERR_MKFIFO_FAILED;

    return rc;
}

static int fsmMknod(int dirfd, const char *path, mode_t mode, dev_t dev)
{
    /* FIX: check S_IFIFO or dev != 0 */
    int rc = mknodat(dirfd, path, (mode & ~07777), dev);

    if (_fsm_debug) {
	rpmlog(RPMLOG_DEBUG, " %8s (%d %s, 0%o, 0x%x) %s\n",
	       __func__, dirfd, path, (unsigned)(mode & ~07777),
	       (unsigned)dev, (rc < 0 ? strerror(errno) : ""));
    }

    if (rc < 0)
	rc = RPMERR_MKNOD_FAILED;

    return rc;
}

static void removeSBITS(const char *path)
{
    struct stat stb;
    if (lstat(path, &stb) == 0 && S_ISREG(stb.st_mode)) {
	if ((stb.st_mode & 06000) != 0) {
	    (void) chmod(path, stb.st_mode & 0777);
	}
#if WITH_CAP
	if (stb.st_mode & (S_IXUSR|S_IXGRP|S_IXOTH)) {
	    (void) cap_set_file(path, NULL);
	}
#endif
    }
}

static void fsmDebug(const char *fpath, rpmFileAction action,
		     const struct stat *st)
{
    rpmlog(RPMLOG_DEBUG, "%-10s %06o%3d (%4d,%4d)%6d %s\n",
	   fileActionString(action), (int)st->st_mode,
	   (int)st->st_nlink, (int)st->st_uid,
	   (int)st->st_gid, (int)st->st_size,
	    (fpath ? fpath : ""));
}

static int fsmSymlink(const char *opath, int dirfd, const char *path)
{
    int rc = symlinkat(opath, dirfd, path);

    if (_fsm_debug) {
	rpmlog(RPMLOG_DEBUG, " %8s (%s, %d %s) %s\n", __func__,
	       opath, dirfd, path, (rc < 0 ? strerror(errno) : ""));
    }

    if (rc < 0)
	rc = RPMERR_SYMLINK_FAILED;
    return rc;
}

static int fsmUnlink(const char *path)
{
    int rc = 0;
    removeSBITS(path);
    rc = unlink(path);
    if (_fsm_debug)
	rpmlog(RPMLOG_DEBUG, " %8s (%s) %s\n", __func__,
	       path, (rc < 0 ? strerror(errno) : ""));
    if (rc < 0)
	rc = (errno == ENOENT ? RPMERR_ENOENT : RPMERR_UNLINK_FAILED);
    return rc;
}

static int fsmRename(const char *opath, const char *path)
{
    removeSBITS(path);
    int rc = rename(opath, path);
#if defined(ETXTBSY) && defined(__HPUX__)
    /* XXX HP-UX (and other os'es) don't permit rename to busy files. */
    if (rc && errno == ETXTBSY) {
	char *rmpath = NULL;
	rstrscat(&rmpath, path, "-RPMDELETE", NULL);
	rc = rename(path, rmpath);
	if (!rc) rc = rename(opath, path);
	free(rmpath);
    }
#endif
    if (_fsm_debug)
	rpmlog(RPMLOG_DEBUG, " %8s (%s, %s) %s\n", __func__,
	       opath, path, (rc < 0 ? strerror(errno) : ""));
    if (rc < 0)
	rc = (errno == EISDIR ? RPMERR_EXIST_AS_DIR : RPMERR_RENAME_FAILED);
    return rc;
}

static int fsmRemove(const char *path, mode_t mode)
{
    return S_ISDIR(mode) ? fsmRmdir(path) : fsmUnlink(path);
}

static int fsmChown(const char *path, mode_t mode, uid_t uid, gid_t gid)
{
    int rc = S_ISLNK(mode) ? lchown(path, uid, gid) : chown(path, uid, gid);
    if (rc < 0) {
	struct stat st;
	if (lstat(path, &st) == 0 && st.st_uid == uid && st.st_gid == gid)
	    rc = 0;
    }
    if (_fsm_debug)
	rpmlog(RPMLOG_DEBUG, " %8s (%s, %d, %d) %s\n", __func__,
	       path, (int)uid, (int)gid,
	       (rc < 0 ? strerror(errno) : ""));
    if (rc < 0)	rc = RPMERR_CHOWN_FAILED;
    return rc;
}

static int fsmChmod(const char *path, mode_t mode)
{
    int rc = chmod(path, (mode & 07777));
    if (rc < 0) {
	struct stat st;
	if (lstat(path, &st) == 0 && (st.st_mode & 07777) == (mode & 07777))
	    rc = 0;
    }
    if (_fsm_debug)
	rpmlog(RPMLOG_DEBUG, " %8s (%s, 0%04o) %s\n", __func__,
	       path, (unsigned)(mode & 07777),
	       (rc < 0 ? strerror(errno) : ""));
    if (rc < 0)	rc = RPMERR_CHMOD_FAILED;
    return rc;
}

static int fsmUtime(const char *path, mode_t mode, time_t mtime)
{
    int rc = 0;
    struct timeval stamps[2] = {
	{ .tv_sec = mtime, .tv_usec = 0 },
	{ .tv_sec = mtime, .tv_usec = 0 },
    };

#if HAVE_LUTIMES
    rc = lutimes(path, stamps);
#else
    if (!S_ISLNK(mode))
	rc = utimes(path, stamps);
#endif
    
    if (_fsm_debug)
	rpmlog(RPMLOG_DEBUG, " %8s (%s, 0x%x) %s\n", __func__,
	       path, (unsigned)mtime, (rc < 0 ? strerror(errno) : ""));
    if (rc < 0)	rc = RPMERR_UTIME_FAILED;
    /* ...but utime error is not critical for directories */
    if (rc && S_ISDIR(mode))
	rc = 0;
    return rc;
}

static int fsmVerify(const char *path, rpmfi fi)
{
    int rc;
    int saveerrno = errno;
    struct stat dsb;
    mode_t mode = rpmfiFMode(fi);

    rc = fsmStat(path, 1, &dsb);
    if (rc)
	return rc;

    if (S_ISREG(mode)) {
	/* HP-UX (and other os'es) don't permit unlink on busy files. */
	char *rmpath = rstrscat(NULL, path, "-RPMDELETE", NULL);
	rc = fsmRename(path, rmpath);
	/* XXX shouldn't we take unlink return code here? */
	if (!rc)
	    (void) fsmUnlink(rmpath);
	else
	    rc = RPMERR_UNLINK_FAILED;
	free(rmpath);
        return (rc ? rc : RPMERR_ENOENT);	/* XXX HACK */
    } else if (S_ISDIR(mode)) {
        if (S_ISDIR(dsb.st_mode)) return 0;
        if (S_ISLNK(dsb.st_mode)) {
	    uid_t luid = dsb.st_uid;
            rc = fsmStat(path, 0, &dsb);
            if (rc == RPMERR_ENOENT) rc = 0;
            if (rc) return rc;
            errno = saveerrno;
	    /* Only permit directory symlinks by target owner and root */
            if (S_ISDIR(dsb.st_mode) && (luid == 0 || luid == dsb.st_uid))
		    return 0;
        }
    } else if (S_ISLNK(mode)) {
        if (S_ISLNK(dsb.st_mode)) {
            char buf[8 * BUFSIZ];
            size_t len;
            rc = fsmReadLink(path, buf, 8 * BUFSIZ, &len);
            errno = saveerrno;
            if (rc) return rc;
            if (rstreq(rpmfiFLink(fi), buf)) return 0;
        }
    } else if (S_ISFIFO(mode)) {
        if (S_ISFIFO(dsb.st_mode)) return 0;
    } else if (S_ISCHR(mode) || S_ISBLK(mode)) {
        if ((S_ISCHR(dsb.st_mode) || S_ISBLK(dsb.st_mode)) &&
            (dsb.st_rdev == rpmfiFRdev(fi))) return 0;
    } else if (S_ISSOCK(mode)) {
        if (S_ISSOCK(dsb.st_mode)) return 0;
    }
    /* XXX shouldn't do this with commit/undo. */
    rc = fsmUnlink(path);
    if (rc == 0)	rc = RPMERR_ENOENT;
    return (rc ? rc : RPMERR_ENOENT);	/* XXX HACK */
}

#define	IS_DEV_LOG(_x)	\
	((_x) != NULL && strlen(_x) >= (sizeof("/dev/log")-1) && \
	rstreqn((_x), "/dev/log", sizeof("/dev/log")-1) && \
	((_x)[sizeof("/dev/log")-1] == '\0' || \
	 (_x)[sizeof("/dev/log")-1] == ';'))



/* Rename pre-existing modified or unmanaged file. */
static int fsmBackup(rpmfi fi, rpmFileAction action)
{
    int rc = 0;
    const char *suffix = NULL;

    if (!(rpmfiFFlags(fi) & RPMFILE_GHOST)) {
	switch (action) {
	case FA_SAVE:
	    suffix = SUFFIX_RPMSAVE;
	    break;
	case FA_BACKUP:
	    suffix = SUFFIX_RPMORIG;
	    break;
	default:
	    break;
	}
    }

    if (suffix) {
	char * opath = fsmFsPath(fi, NULL);
	char * path = fsmFsPath(fi, suffix);
	rc = fsmRename(opath, path);
	if (!rc) {
	    rpmlog(RPMLOG_WARNING, _("%s saved as %s\n"), opath, path);
	}
	free(path);
	free(opath);
    }
    return rc;
}

static int fsmSetmeta(const char *path, rpmfi fi, rpmPlugins plugins,
		      rpmFileAction action, const struct stat * st,
		      int nofcaps)
{
    int rc = 0;
    const char *dest = rpmfiFN(fi);

    if (!rc && !getuid()) {
	rc = fsmChown(path, st->st_mode, st->st_uid, st->st_gid);
    }
    if (!rc && !S_ISLNK(st->st_mode)) {
	rc = fsmChmod(path, st->st_mode);
    }
    /* Set file capabilities (if enabled) */
    if (!rc && !nofcaps && S_ISREG(st->st_mode) && !getuid()) {
	rc = fsmSetFCaps(path, rpmfiFCaps(fi));
    }
    if (!rc) {
	rc = fsmUtime(path, st->st_mode, rpmfiFMtime(fi));
    }
    if (!rc) {
	rc = rpmpluginsCallFsmFilePrepare(plugins, fi,
					  path, dest, st->st_mode, action);
    }

    return rc;
}

static int fsmCommit(char **path, rpmfi fi, rpmFileAction action, const char *suffix)
{
    int rc = 0;

    /* XXX Special case /dev/log, which shouldn't be packaged anyways */
    if (!(S_ISSOCK(rpmfiFMode(fi)) && IS_DEV_LOG(*path))) {
	const char *nsuffix = (action == FA_ALTNAME) ? SUFFIX_RPMNEW : NULL;
	char *dest = *path;
	/* Construct final destination path (nsuffix is usually NULL) */
	if (suffix)
	    dest = fsmFsPath(fi, nsuffix);

	/* Rename temporary to final file name if needed. */
	if (dest != *path) {
	    rc = fsmRename(*path, dest);
	    if (!rc) {
		if (nsuffix) {
		    char * opath = fsmFsPath(fi, NULL);
		    rpmlog(RPMLOG_WARNING, _("%s created as %s\n"),
			   opath, dest);
		    free(opath);
		}
		free(*path);
		*path = dest;
	    }
	}
    }

    return rc;
}

/**
 * Return formatted string representation of file disposition.
 * @param a		file disposition
 * @return		formatted string
 */
static const char * fileActionString(rpmFileAction a)
{
    switch (a) {
    case FA_UNKNOWN:	return "unknown";
    case FA_CREATE:	return "create";
    case FA_BACKUP:	return "backup";
    case FA_SAVE:	return "save";
    case FA_SKIP:	return "skip";
    case FA_ALTNAME:	return "altname";
    case FA_ERASE:	return "erase";
    case FA_SKIPNSTATE: return "skipnstate";
    case FA_SKIPNETSHARED: return "skipnetshared";
    case FA_SKIPCOLOR:	return "skipcolor";
    case FA_TOUCH:     return "touch";
    default:		return "???";
    }
}

/* Remember any non-regular file state for recording in the rpmdb */
static void setFileState(rpmfs fs, int i)
{
    switch (rpmfsGetAction(fs, i)) {
    case FA_SKIPNSTATE:
	rpmfsSetState(fs, i, RPMFILE_STATE_NOTINSTALLED);
	break;
    case FA_SKIPNETSHARED:
	rpmfsSetState(fs, i, RPMFILE_STATE_NETSHARED);
	break;
    case FA_SKIPCOLOR:
	rpmfsSetState(fs, i, RPMFILE_STATE_WRONGCOLOR);
	break;
    case FA_TOUCH:
	rpmfsSetState(fs, i, RPMFILE_STATE_NORMAL);
	break;
    default:
	break;
    }
}

struct diriter_s {
    int dirfd;
};

static int onChdir(rpmfi fi, void *data)
{
    struct diriter_s *di = data;

    if (di->dirfd >= 0) {
	close(di->dirfd);
	di->dirfd = -1;
    }
    return 0;
}

static rpmfi fsmIter(FD_t payload, rpmfiles files, rpmFileIter iter, void *data)
{
    rpmfi fi;
    if (payload)
	fi = rpmfiNewArchiveReader(payload, files, RPMFI_ITER_READ_ARCHIVE);
    else
	fi = rpmfilesIter(files, iter);
    if (fi && data)
	rpmfiSetOnChdir(fi, onChdir, data);
    return fi;
}

static rpmfi fsmIterFini(rpmfi fi, struct diriter_s *di)
{
    close(di->dirfd);
    di->dirfd = -1;
    return rpmfiFree(fi);
}

int rpmPackageFilesInstall(rpmts ts, rpmte te, rpmfiles files,
              rpmpsm psm, char ** failedFile)
{
    FD_t payload = rpmtePayload(te);
    rpmfi fi = NULL;
    rpmfs fs = rpmteGetFileStates(te);
    rpmPlugins plugins = rpmtsPlugins(ts);
    int rc = 0;
    int fx = -1;
    int fc = rpmfilesFC(files);
    int nodigest = (rpmtsFlags(ts) & RPMTRANS_FLAG_NOFILEDIGEST) ? 1 : 0;
    int nofcaps = (rpmtsFlags(ts) & RPMTRANS_FLAG_NOCAPS) ? 1 : 0;
    FD_t firstlinkfile = NULL;
    char *tid = NULL;
    struct filedata_s *fdata = xcalloc(fc, sizeof(*fdata));
    struct filedata_s *firstlink = NULL;
    struct diriter_s di = { -1 };

    /* transaction id used for temporary path suffix while installing */
    rasprintf(&tid, ";%08x", (unsigned)rpmtsGetTid(ts));

    /* Collect state data for the whole operation */
    fi = rpmfilesIter(files, RPMFI_ITER_FWD);
    while (!rc && (fx = rpmfiNext(fi)) >= 0) {
	struct filedata_s *fp = &fdata[fx];
	if (rpmfiFFlags(fi) & RPMFILE_GHOST)
            fp->action = FA_SKIP;
	else
	    fp->action = rpmfsGetAction(fs, fx);
	fp->skip = XFA_SKIPPING(fp->action);
	if (XFA_CREATING(fp->action) && !S_ISDIR(rpmfiFMode(fi)))
	    fp->suffix = tid;
	fp->fpath = fsmFsPath(fi, fp->suffix);

	/* Remap file perms, owner, and group. */
	rc = rpmfiStat(fi, 1, &fp->sb);

	/* Hardlinks are tricky and handled elsewhere for install */
	fp->setmeta = (fp->skip == 0) &&
		      (fp->sb.st_nlink == 1 || fp->action == FA_TOUCH);

	setFileState(fs, fx);
	fsmDebug(fp->fpath, fp->action, &fp->sb);

	fp->stage = FILE_PRE;
    }
    fi = rpmfiFree(fi);

    if (rc)
	goto exit;

    fi = fsmIter(payload, files,
		 payload ? RPMFI_ITER_READ_ARCHIVE : RPMFI_ITER_FWD, &di);

    if (fi == NULL) {
        rc = RPMERR_BAD_MAGIC;
        goto exit;
    }

    /* Process the payload */
    while (!rc && (fx = rpmfiNext(fi)) >= 0) {
	struct filedata_s *fp = &fdata[fx];

	/*
	 * Tricksy case: this file is a being skipped, but it's part of
	 * a hardlinked set and has the actual content linked with it.
	 * Write the content to the first non-skipped file of the set
	 * instead.
	 */
	if (fp->skip && firstlink && rpmfiArchiveHasContent(fi))
	    fp = firstlink;

        if (!fp->skip) {
	    /* Directories replacing something need early backup */
	    if (!fp->suffix && fp != firstlink) {
		rc = fsmBackup(fi, fp->action);
	    }

	    if (!rc) {
		rc = ensureDir(plugins, rpmfiDN(fi), 0,
				(fp->action == FA_CREATE), &di.dirfd);
	    }

	    /* Run fsm file pre hook for all plugins */
	    rc = rpmpluginsCallFsmFilePre(plugins, fi, fp->fpath,
					  fp->sb.st_mode, fp->action);
	    if (rc)
		goto setmeta; /* for error notification */

	    /* Assume file does't exist when tmp suffix is in use */
	    if (!fp->suffix) {
		if (fp->action == FA_TOUCH) {
		    struct stat sb;
		    rc = fsmStat(fp->fpath, 1, &sb);
		} else {
		    rc = fsmVerify(fp->fpath, fi);
		}
	    } else {
		rc = RPMERR_ENOENT;
	    }

	    /* See if the file was removed while our attention was elsewhere */
	    if (rc == RPMERR_ENOENT && fp->action == FA_TOUCH) {
		rpmlog(RPMLOG_DEBUG, "file %s vanished unexpectedly\n",
			fp->fpath);
		fp->action = FA_CREATE;
		fsmDebug(fp->fpath, fp->action, &fp->sb);
	    }

	    /* When touching we don't need any of this... */
	    if (fp->action == FA_TOUCH)
		goto setmeta;

            if (S_ISREG(fp->sb.st_mode)) {
		if (rc == RPMERR_ENOENT) {
		    rc = fsmMkfile(di.dirfd, fi, fp, files, psm, nodigest,
				   &firstlink, &firstlinkfile);
		}
            } else if (S_ISDIR(fp->sb.st_mode)) {
                if (rc == RPMERR_ENOENT) {
                    mode_t mode = fp->sb.st_mode;
                    mode &= ~07777;
                    mode |=  00700;
                    rc = fsmMkdir(di.dirfd, fp->fpath, mode);
                }
            } else if (S_ISLNK(fp->sb.st_mode)) {
		if (rc == RPMERR_ENOENT) {
		    rc = fsmSymlink(rpmfiFLink(fi), di.dirfd, fp->fpath);
		}
            } else if (S_ISFIFO(fp->sb.st_mode)) {
                /* This mimics cpio S_ISSOCK() behavior but probably isn't right */
                if (rc == RPMERR_ENOENT) {
                    rc = fsmMkfifo(di.dirfd, fp->fpath, 0000);
                }
            } else if (S_ISCHR(fp->sb.st_mode) ||
                       S_ISBLK(fp->sb.st_mode) ||
                       S_ISSOCK(fp->sb.st_mode))
            {
                if (rc == RPMERR_ENOENT) {
                    rc = fsmMknod(di.dirfd, fp->fpath, fp->sb.st_mode, fp->sb.st_rdev);
                }
            } else {
                /* XXX Special case /dev/log, which shouldn't be packaged anyways */
                if (!IS_DEV_LOG(fp->fpath))
                    rc = RPMERR_UNKNOWN_FILETYPE;
            }

setmeta:
	    if (!rc && fp->setmeta) {
		rc = fsmSetmeta(fp->fpath, fi, plugins, fp->action,
				&fp->sb, nofcaps);
	    }
	}

	/* Notify on success. */
	if (rc)
	    *failedFile = xstrdup(fp->fpath);
	else
	    rpmpsmNotify(psm, RPMCALLBACK_INST_PROGRESS, rpmfiArchiveTell(fi));
	fp->stage = FILE_UNPACK;
    }
    fi = fsmIterFini(fi, &di);

    if (!rc && fx < 0 && fx != RPMERR_ITER_END)
	rc = fx;

    /* If all went well, commit files to final destination */
    fi = rpmfilesIter(files, RPMFI_ITER_FWD);
    while (!rc && (fx = rpmfiNext(fi)) >= 0) {
	struct filedata_s *fp = &fdata[fx];

	if (!fp->skip) {
	    /* Backup file if needed. Directories are handled earlier */
	    if (!rc && fp->suffix)
		rc = fsmBackup(fi, fp->action);

	    if (!rc)
		rc = fsmCommit(&fp->fpath, fi, fp->action, fp->suffix);

	    if (!rc)
		fp->stage = FILE_COMMIT;
	    else
		*failedFile = xstrdup(fp->fpath);

	    /* Run fsm file post hook for all plugins for all processed files */
	    rpmpluginsCallFsmFilePost(plugins, fi, fp->fpath,
				      fp->sb.st_mode, fp->action, rc);
	}
    }
    fi = rpmfiFree(fi);

    /* On failure, walk backwards and erase non-committed files */
    if (rc) {
	fi = rpmfilesIter(files, RPMFI_ITER_BACK);
	while ((fx = rpmfiNext(fi)) >= 0) {
	    struct filedata_s *fp = &fdata[fx];

	    if (fp->stage > FILE_NONE && !fp->skip) {
		(void) fsmRemove(fp->fpath, fp->sb.st_mode);
	    }
	}
    }

    rpmswAdd(rpmtsOp(ts, RPMTS_OP_UNCOMPRESS), fdOp(payload, FDSTAT_READ));
    rpmswAdd(rpmtsOp(ts, RPMTS_OP_DIGEST), fdOp(payload, FDSTAT_DIGEST));

exit:
    fi = rpmfiFree(fi);
    Fclose(payload);
    free(tid);
    for (int i = 0; i < fc; i++)
	free(fdata[i].fpath);
    free(fdata);

    return rc;
}


int rpmPackageFilesRemove(rpmts ts, rpmte te, rpmfiles files,
              rpmpsm psm, char ** failedFile)
{
    rpmfi fi = rpmfilesIter(files, RPMFI_ITER_BACK);
    rpmfs fs = rpmteGetFileStates(te);
    rpmPlugins plugins = rpmtsPlugins(ts);
    int fc = rpmfilesFC(files);
    int fx = -1;
    struct filedata_s *fdata = xcalloc(fc, sizeof(*fdata));
    int rc = 0;

    while (!rc && (fx = rpmfiNext(fi)) >= 0) {
	struct filedata_s *fp = &fdata[fx];
	fp->action = rpmfsGetAction(fs, rpmfiFX(fi));
	fp->fpath = fsmFsPath(fi, NULL);
	rc = fsmStat(fp->fpath, 1, &fp->sb);

	fsmDebug(fp->fpath, fp->action, &fp->sb);

	/* Run fsm file pre hook for all plugins */
	rc = rpmpluginsCallFsmFilePre(plugins, fi, fp->fpath,
				      fp->sb.st_mode, fp->action);

	if (!XFA_SKIPPING(fp->action))
	    rc = fsmBackup(fi, fp->action);

        /* Remove erased files. */
        if (fp->action == FA_ERASE) {
	    int missingok = (rpmfiFFlags(fi) & (RPMFILE_MISSINGOK | RPMFILE_GHOST));

	    rc = fsmRemove(fp->fpath, fp->sb.st_mode);

	    /*
	     * Missing %ghost or %missingok entries are not errors.
	     * XXX: Are non-existent files ever an actual error here? Afterall
	     * that's exactly what we're trying to accomplish here,
	     * and complaining about job already done seems like kinderkarten
	     * level "But it was MY turn!" whining...
	     */
	    if (rc == RPMERR_ENOENT && missingok) {
		rc = 0;
	    }

	    /*
	     * Dont whine on non-empty directories for now. We might be able
	     * to track at least some of the expected failures though,
	     * such as when we knowingly left config file backups etc behind.
	     */
	    if (rc == RPMERR_ENOTEMPTY) {
		rc = 0;
	    }

	    if (rc) {
		int lvl = strict_erasures ? RPMLOG_ERR : RPMLOG_WARNING;
		rpmlog(lvl, _("%s %s: remove failed: %s\n"),
			S_ISDIR(fp->sb.st_mode) ? _("directory") : _("file"),
			fp->fpath, strerror(errno));
            }
        }

	/* Run fsm file post hook for all plugins */
	rpmpluginsCallFsmFilePost(plugins, fi, fp->fpath,
				  fp->sb.st_mode, fp->action, rc);

        /* XXX Failure to remove is not (yet) cause for failure. */
        if (!strict_erasures) rc = 0;

	if (rc)
	    *failedFile = xstrdup(fp->fpath);

	if (rc == 0) {
	    /* Notify on success. */
	    /* On erase we're iterating backwards, fixup for progress */
	    rpm_loff_t amount = rpmfiFC(fi) - rpmfiFX(fi);
	    rpmpsmNotify(psm, RPMCALLBACK_UNINST_PROGRESS, amount);
	}
    }

    for (int i = 0; i < fc; i++)
	free(fdata[i].fpath);
    free(fdata);
    rpmfiFree(fi);

    return rc;
}


