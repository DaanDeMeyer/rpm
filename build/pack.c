/* RPM - Copyright (C) 1995 Red Hat Software
 * 
 * pack.c - routines for packaging
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <ftw.h>
#include <netinet/in.h>
#include <pwd.h>
#include <grp.h>
#include <netdb.h>

#include "header.h"
#include "specP.h"
#include "rpmerr.h"
#include "rpmlead.h"
#include "rpmlib.h"
#include "misc.h"
#include "pack.h"
#include "messages.h"
#include "md5.h"
#include "var.h"

#define BINARY_HEADER 0
#define SOURCE_HEADER 1

struct file_entry {
    char file[1024];
    int isdoc;
    int isconf;
    char *uname;  /* reference -- do not free */
    char *gname;  /* reference -- do not free */
    struct stat statbuf;
    struct file_entry *next;
};

static int cpio_gzip(Header header, int fd, char *tempdir);
static int writeMagic(Spec s, int fd, char *name, unsigned short type);
static int add_file(struct file_entry **festack,
		    char *name, int isdoc, int isconf, int isdir);
static int compare_fe(const void *ap, const void *bp);
static int process_filelist(Header header, StringBuf sb, int *size, int type);
static char *buildHost(void);
static int add_file_aux(char *file, struct stat *sb, int flag);
static char *getUname(uid_t uid);
static char *getGname(gid_t gid);

static void resetDocdir(void);
static void addDocdir(char *dirname);
static int isDoc(char *filename);

static int writeMagic(Spec s, int fd, char *name, unsigned short type)
{
    struct rpmlead lead;

    lead.major = 2;
    lead.minor = 0;
    lead.type = type;
    lead.archnum = getArchNum();
    lead.osnum = getOsNum();
    lead.signature_type = RPMLEAD_SIGNONE;
    strncpy(lead.name, name, sizeof(lead.name));

    writeLead(fd, &lead);

    return 0;
}

static int cpio_gzip(Header header, int fd, char *tempdir)
{
    char **f, *s;
    int count;
    FILE *inpipeF;
    int cpioPID, gzipPID;
    int inpipe[2];
    int outpipe[2];
    int status;

    pipe(inpipe);
    pipe(outpipe);
    
    if (!(cpioPID = fork())) {
	close(0);
	close(1);
	close(inpipe[1]);
	close(outpipe[0]);
	close(fd);
	
	dup2(inpipe[0], 0);  /* Make stdin the in pipe */
	dup2(outpipe[1], 1); /* Make stdout the out pipe */

	if (tempdir) {
	    chdir(tempdir);
	} else if (getVar(RPMVAR_ROOT)) {
	    if (chdir(getVar(RPMVAR_ROOT))) {
		error(RPMERR_EXEC, "Couldn't chdir to %s",
		      getVar(RPMVAR_ROOT));
		exit(RPMERR_EXEC);
	    }
	} else {
	    chdir("/");
	}

	execlp("cpio", "cpio",
	       (isVerbose()) ? "-ov" : "-o",
	       (tempdir) ? "-LH" : "-H",
	       "crc", NULL);
	error(RPMERR_EXEC, "Couldn't exec cpio");
	exit(RPMERR_EXEC);
    }
    if (cpioPID < 0) {
	error(RPMERR_FORK, "Couldn't fork");
	return RPMERR_FORK;
    }

    if (!(gzipPID = fork())) {
	close(0);
	close(1);
	close(inpipe[1]);
	close(inpipe[0]);
	close(outpipe[1]);

	dup2(outpipe[0], 0); /* Make stdin the out pipe */
	dup2(fd, 1);         /* Make stdout the passed-in file descriptor */
	close(fd);

	execlp("gzip", "gzip", "-c9fn", NULL);
	error(RPMERR_EXEC, "Couldn't exec gzip");
	exit(RPMERR_EXEC);
    }
    if (gzipPID < 0) {
	error(RPMERR_FORK, "Couldn't fork");
	return RPMERR_FORK;
    }

    close(inpipe[0]);
    close(outpipe[1]);
    close(outpipe[0]);

    if (getEntry(header, RPMTAG_FILENAMES, NULL, (void **) &f, &count)) {
	inpipeF = fdopen(inpipe[1], "w");
	while (count--) {
	    s = *f++;
	    /* For binary package, strip the leading "/" for cpio */
	    fprintf(inpipeF, "%s\n", (tempdir) ? s : (s+1));
	}
	fclose(inpipeF);
    } else {
	close(inpipe[1]);
    }

    waitpid(cpioPID, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
	error(RPMERR_CPIO, "cpio failed");
	return 1;
    }
    waitpid(gzipPID, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
	error(RPMERR_GZIP, "gzip failed");
	return 1;
    }

    return 0;
}

/* XXX hard coded limit -- only 1024 %docdir allowed */
static char *docdirs[1024];
static int docdir_count;

static void resetDocdir(void)
{
    while (docdir_count--) {
        free(docdirs[docdir_count]);
    }
    docdir_count = 0;
    docdirs[docdir_count++] = strdup("/usr/doc");
    docdirs[docdir_count++] = strdup("/usr/man");
    docdirs[docdir_count++] = strdup("/usr/info");
}

static void addDocdir(char *dirname)
{
    if (docdir_count == 1024) {
	fprintf(stderr, "RPMERR_INTERNAL: Hit limit in addDocdir()\n");
	exit(RPMERR_INTERNAL);
    }
    docdirs[docdir_count++] = strdup(dirname);
}

static int isDoc(char *filename)
{
    int x = 0;

    while (x < docdir_count) {
        if (strstr(filename, docdirs[x]) == filename) {
	    return 1;
        }
	x++;
    }
    return 0;
}

static char *getUname(uid_t uid)
{
    static uid_t uids[1024];
    static char *unames[1024];
    static int used = 0;
    
    struct passwd *pw;
    int x;

    x = 0;
    while (x < used) {
	if (uids[x] == uid) {
	    return unames[x];
	}
	x++;
    }

    /* XXX - This is the other hard coded limit */
    if (x == 1024) {
	fprintf(stderr, "RPMERR_INTERNAL: Hit limit in getUname()\n");
	exit(RPMERR_INTERNAL);
    }
    
    pw = getpwuid(uid);
    uids[x] = uid;
    used++;
    if (pw) {
	unames[x] = strdup(pw->pw_name);
    } else {
	unames[x] = "";
    }
    return unames[x];
}

static char *getGname(gid_t gid)
{
    static gid_t gids[1024];
    static char *gnames[1024];
    static int used = 0;
    
    struct group *gr;
    int x;

    x = 0;
    while (x < used) {
	if (gids[x] == gid) {
	    return gnames[x];
	}
	x++;
    }

    /* XXX - This is the other hard coded limit */
    if (x == 1024) {
	fprintf(stderr, "RPMERR_INTERNAL: Hit limit in getGname()\n");
	exit(RPMERR_INTERNAL);
    }
    
    gr = getgrgid(gid);
    gids[x] = gid;
    used++;
    if (gr) {
	gnames[x] = strdup(gr->gr_name);
    } else {
	gnames[x] = "";
    }
    return gnames[x];
}

/* Need three globals to keep track of things in ftw() */
static int Gisdoc;
static int Gisconf;
static int Gcount;
static struct file_entry **Gfestack;

static int add_file(struct file_entry **festack,
		    char *name, int isdoc, int isconf, int isdir)
{
    struct file_entry *p;
    char fullname[1024];

    /* Set these up for ftw() */
    Gfestack = festack;
    Gisdoc = isdoc;
    Gisconf = isconf;

    /* XXX do globbing and %doc expansion here */

    p = malloc(sizeof(struct file_entry));
    strcpy(p->file, name);
    p->isdoc = isdoc;
    p->isconf = isconf;
    if (getVar(RPMVAR_ROOT)) {
	sprintf(fullname, "%s%s", getVar(RPMVAR_ROOT), name);
    } else {
	strcpy(fullname, name);
    }
    if (lstat(fullname, &p->statbuf)) {
	return 0;
    }
    p->uname = getUname(p->statbuf.st_uid);
    p->gname = getGname(p->statbuf.st_gid);

    if ((! isdir) && S_ISDIR(p->statbuf.st_mode)) {
	/* This means we need to decend with ftw() */
	Gcount = 0;

	ftw(fullname, add_file_aux, 16);
	
	free(p);

	return Gcount;
    } else {
	/* Link it in */
	p->next = *festack;
	*festack = p;

	message(MESS_DEBUG, "ADDING: %s\n", name);

	/* return number of entries added */
	return 1;
    }
}

static int add_file_aux(char *file, struct stat *sb, int flag)
{
    char *name = file;

    if (getVar(RPMVAR_ROOT)) {
	name += strlen(getVar(RPMVAR_ROOT));
    }

    /* The 1 will cause add_file() to *not* descend */
    /* directories -- ftw() is already doing it!    */
    Gcount += add_file(Gfestack, name, Gisdoc, Gisconf, 1);

    return 0; /* for ftw() */
}

static int compare_fe(const void *ap, const void *bp)
{
    char *a, *b;

    a = (*(struct file_entry **)ap)->file;
    b = (*(struct file_entry **)bp)->file;

    return strcmp(b, a);
}

static int process_filelist(Header header, StringBuf sb, int *size, int type)
{
    char buf[1024];
    char **files, **fp;
    struct file_entry *fes, *fest;
    struct file_entry **file_entry_array;
    int isdoc, isconf, isdir;
    char *filename, *s;
    char *str;
    int count = 0;
    int c;

    fes = NULL;
    *size = 0;

    resetDocdir();
    
    str = getStringBuf(sb);
    files = splitString(str, strlen(str), '\n');
    fp = files;

    while (*fp) {
	strcpy(buf, *fp);  /* temp copy */
	isdoc = 0;
	isconf = 0;
	isdir = 0;
	filename = NULL;
	s = strtok(buf, " \t\n");
	while (s) {
	    if (!strcmp(s, "%doc")) {
		isdoc = 1;
	    } else if (!strcmp(s, "%config")) {
		isconf = 1;
	    } else if (!strcmp(s, "%dir")) {
		isdir = 1;
	    } else if (!strcmp(s, "%docdir")) {
	        s = strtok(NULL, " \t\n");
		addDocdir(s);
		break;
	    } else {
		filename = s;
	    }
	    s = strtok(NULL, " \t\n");
	}
	if (! filename) {
	    fp++;
	    continue;
	}

	if (type == RPMLEAD_BINARY) {
	    /* check that file starts with leading "/" */
	    if (*filename != '/') {
		error(RPMERR_BADSPEC,
		      "File needs leading \"/\": %s", filename);
		return(RPMERR_BADSPEC);
	    }

	    c = add_file(&fes, filename, isdoc, isconf, isdir);
	} else {
	    /* Source package are the simple case */
	    fest = malloc(sizeof(struct file_entry));
	    fest->isdoc = 0;
	    fest->isconf = 0;
	    stat(filename, &fest->statbuf);
	    fest->uname = getUname(fest->statbuf.st_uid);
	    fest->gname = getGname(fest->statbuf.st_gid);
	    strcpy(fest->file, filename);
	    fest->next = fes;
	    fes = fest;
	    c = 1;
	}
	    
	if (! c) {
	    error(RPMERR_BADSPEC, "File not found: %s", filename);
	    return(RPMERR_BADSPEC);
	}
	count += c;
	
	fp++;
    }

    /* If there are no files, don't add anything to the header */
    if (count) {
	char ** fileList;
	char ** fileMD5List;
	char ** fileLinktoList;
	int_32 * fileSizeList;
	int_32 * fileUIDList;
	int_32 * fileGIDList;
	char ** fileUnameList;
	char ** fileGnameList;
	int_32 * fileMtimesList;
	int_32 * fileFlagsList;
	int_16 * fileModesList;
	int_16 * fileRDevsList;

	fileList = malloc(sizeof(char *) * count);
	fileLinktoList = malloc(sizeof(char *) * count);
	fileMD5List = malloc(sizeof(char *) * count);
	fileSizeList = malloc(sizeof(int_32) * count);
	fileUIDList = malloc(sizeof(int_32) * count);
	fileGIDList = malloc(sizeof(int_32) * count);
	fileUnameList = malloc(sizeof(char *) * count);
	fileGnameList = malloc(sizeof(char *) * count);
	fileMtimesList = malloc(sizeof(int_32) * count);
	fileFlagsList = malloc(sizeof(int_32) * count);
	fileModesList = malloc(sizeof(int_16) * count);
	fileRDevsList = malloc(sizeof(int_16) * count);

	/* Build a reverse sorted file array.  */
	/* This makes uninstalls a lot easier. */
	file_entry_array = malloc(sizeof(struct file_entry *) * count);
	c = 0;
	fest = fes;
	while (fest) {
	    file_entry_array[c++] = fest;
	    fest = fest->next;
	}
	qsort(file_entry_array, count, sizeof(struct file_entry *), compare_fe);
	
	c = 0;
	while (c < count) {
	    fest = file_entry_array[c];
	    if (type == RPMLEAD_BINARY) {
		fileList[c] = fest->file;
	    } else {
		fileList[c] = strrchr(fest->file, '/') + 1;
	    }
	    fileUnameList[c] = fest->uname;
	    fileGnameList[c] = fest->gname;
	    *size += fest->statbuf.st_size;
	    if (S_ISREG(fest->statbuf.st_mode)) {
		mdfile(fest->file, buf);
		fileMD5List[c] = strdup(buf);
		message(MESS_DEBUG, "md5(%s) = %s\n", fest->file, buf);
	    } else {
		/* This is stupid */
		fileMD5List[c] = strdup("");
	    }
	    fileSizeList[c] = fest->statbuf.st_size;
	    fileUIDList[c] = fest->statbuf.st_uid;
	    fileGIDList[c] = fest->statbuf.st_gid;
	    fileMtimesList[c] = fest->statbuf.st_mtime;
	    fileFlagsList[c] = 0;
	    if (isDoc(fest->file))
	        fileFlagsList[c] |= RPMFILE_DOC;
	    if (fest->isdoc) 
		fileFlagsList[c] |= RPMFILE_DOC;
	    if (fest->isconf)
		fileFlagsList[c] |= RPMFILE_CONFIG;

	    fileModesList[c] = fest->statbuf.st_mode;
	    fileRDevsList[c] = fest->statbuf.st_rdev;

	    if (S_ISLNK(fest->statbuf.st_mode)) {
		if (getVar(RPMVAR_ROOT)) {
		    sprintf(buf, "%s%s", getVar(RPMVAR_ROOT), fest->file);
		} else {
		    strcpy(buf, fest->file);
		}
		readlink(buf, buf, 1024);
		fileLinktoList[c] = strdup(buf);
	    } else {
		/* This is stupid */
		fileLinktoList[c] = strdup("");
	    }
	    c++;
	}

	/* Add the header entries */
	c = count;
	addEntry(header, RPMTAG_FILENAMES, STRING_ARRAY_TYPE, fileList, c);
	addEntry(header, RPMTAG_FILELINKTOS, STRING_ARRAY_TYPE,
		 fileLinktoList, c);
	addEntry(header, RPMTAG_FILEMD5S, STRING_ARRAY_TYPE, fileMD5List, c);
	addEntry(header, RPMTAG_FILESIZES, INT32_TYPE, fileSizeList, c);
	addEntry(header, RPMTAG_FILEUIDS, INT32_TYPE, fileUIDList, c);
	addEntry(header, RPMTAG_FILEGIDS, INT32_TYPE, fileGIDList, c);
	addEntry(header, RPMTAG_FILEUSERNAME, STRING_ARRAY_TYPE,
		 fileUnameList, c);
	addEntry(header, RPMTAG_FILEGROUPNAME, STRING_ARRAY_TYPE,
		 fileGnameList, c);
	addEntry(header, RPMTAG_FILEMTIMES, INT32_TYPE, fileMtimesList, c);
	addEntry(header, RPMTAG_FILEFLAGS, INT32_TYPE, fileFlagsList, c);
	addEntry(header, RPMTAG_FILEMODES, INT16_TYPE, fileModesList, c);
	addEntry(header, RPMTAG_FILERDEVS, INT16_TYPE, fileRDevsList, c);
	
	/* Free the allocated strings */
	c = count;
	while (c--) {
	    free(fileMD5List[c]);
	    free(fileLinktoList[c]);
	}

	/* Free the file entry array */
	free(file_entry_array);
	
	/* Free the file entry stack */
	fest = fes;
	while (fest) {
	    fes = fest->next;
	    free(fest);
	    fest = fes;
	}
    }
    
    freeSplitString(files);
    return 0;
}

static time_t buildtime;
void markBuildTime(void)
{
    buildtime = time(NULL);
}

static char *buildHost(void)
{
    static char hostname[1024];
    static int gotit = 0;
    struct hostent *hbn;

    if (! gotit) {
        gethostname(hostname, sizeof(hostname));
	hbn = gethostbyname(hostname);
	strcpy(hostname, hbn->h_name);
	gotit = 1;
    }
    return(hostname);
}

int packageBinaries(Spec s)
{
    char name[1024];
    char filename[1024];
    char *icon;
    int iconFD;
    struct stat statbuf;
    struct PackageRec *pr;
    Header outHeader;
    HeaderIterator headerIter;
    int_32 tag, type, c;
    void *ptr;
    int fd;
    char *version;
    char *release;
    int size;
    int_8 os, arch;

    if (!getEntry(s->packages->header, RPMTAG_VERSION, NULL,
		  (void *) &version, NULL)) {
	error(RPMERR_BADSPEC, "No version field");
	return RPMERR_BADSPEC;
    }
    if (!getEntry(s->packages->header, RPMTAG_RELEASE, NULL,
		  (void *) &release, NULL)) {
	error(RPMERR_BADSPEC, "No release field");
	return RPMERR_BADSPEC;
    }

    /* Look through for each package */
    pr = s->packages;
    while (pr) {
	if (pr->files == -1) {
	    pr = pr->next;
	    continue;
	}
	
	if (pr->subname) {
	    strcpy(name, s->name);
	    strcat(name, "-");
	    strcat(name, pr->subname);
	} else if (pr->newname) {
	    strcpy(name, pr->newname);
	} else {
	    strcpy(name, s->name);
	}
	strcat(name, "-");
	strcat(name, version);
	strcat(name, "-");
	strcat(name, release);
	
	/* First build the header structure.            */
	/* Here's the plan: copy the package's header,  */
	/* then add entries from the primary header     */
	/* that don't already exist.                    */
	outHeader = copyHeader(pr->header);
	headerIter = initIterator(s->packages->header);
	while (nextIterator(headerIter, &tag, &type, &ptr, &c)) {
	    /* Some tags we don't copy */
	    switch (tag) {
	      case RPMTAG_PREIN:
	      case RPMTAG_POSTIN:
	      case RPMTAG_PREUN:
	      case RPMTAG_POSTUN:
		  continue;
		  break;  /* Shouldn't need this */
	      default:
		  if (! isEntry(outHeader, tag)) {
		      addEntry(outHeader, tag, type, ptr, c);
		  }
	    }
	}
	freeIterator(headerIter);
	
	if (process_filelist(outHeader, pr->filelist, &size, RPMLEAD_BINARY)) {
	    return 1;
	}
	
	sprintf(filename, "%s.%s.rpm", name, getArchName());
	fd = open(filename, O_WRONLY|O_CREAT|O_TRUNC, 0644);
	if (writeMagic(s, fd, name, RPMLEAD_BINARY)) {
	    return 1;
	}

	/* Add some final entries to the header */
	os = getArchNum();
	arch = getArchNum();
	addEntry(outHeader, RPMTAG_OS, INT8_TYPE, &os, 1);
	addEntry(outHeader, RPMTAG_ARCH, INT8_TYPE, &arch, 1);
	addEntry(outHeader, RPMTAG_BUILDTIME, INT32_TYPE, &buildtime, 1);
	addEntry(outHeader, RPMTAG_SIZE, INT32_TYPE, &size, 1);
	addEntry(outHeader, RPMTAG_BUILDHOST, STRING_TYPE, buildHost(), 1);
	if (pr->icon) {
	    sprintf(filename, "%s/%s", getVar(RPMVAR_SOURCEDIR), pr->icon);
	    stat(filename, &statbuf);
	    icon = malloc(statbuf.st_size);
	    iconFD = open(filename, O_RDONLY, 0644);
	    read(iconFD, icon, statbuf.st_size);
	    close(iconFD);
	    if (! strncmp(icon, "GIF", 3)) {
		addEntry(outHeader, RPMTAG_GIF, BIN_TYPE,
			 icon, statbuf.st_size);
	    } else if (! strncmp(icon, "/* XPM", 6)) {
		addEntry(outHeader, RPMTAG_XPM, BIN_TYPE,
			 icon, statbuf.st_size);
	    } else {
		addEntry(outHeader, RPMTAG_ICON, BIN_TYPE,
			 icon, statbuf.st_size);
	    }
	    free(icon);
	}
	/* XXX - need: distribution, vendor, release */
	
	writeHeader(fd, outHeader);
	
	/* Now do the cpio | gzip thing */
	if (cpio_gzip(outHeader, fd, NULL)) {
	    return 1;
	}
    
	close(fd);

	freeHeader(outHeader);
	pr = pr->next;
    }
    
    return 0;
}

/**************** SOURCE PACKAGING ************************/

int packageSource(Spec s)
{
    struct sources *source;
    struct PackageRec *package;
    char *tempdir;
    char src[1024], dest[1024], filename[1024];
    char *version;
    char *release;
    Header outHeader;
    StringBuf filelist;
    int fd;
    int size;
    int_8 os, arch;

    tempdir = tempnam("/usr/tmp", "rpmbuild");
    mkdir(tempdir, 0700);

    filelist = newStringBuf();
    
    /* Link in the spec file and all the sources */
    sprintf(dest, "%s%s", tempdir, strrchr(s->specfile, '/'));
    symlink(s->specfile, dest);
    appendLineStringBuf(filelist, dest);
    source = s->sources;
    while (source) {
	sprintf(src, "%s/%s", getVar(RPMVAR_SOURCEDIR), source->source);
	sprintf(dest, "%s/%s", tempdir, source->source);
	symlink(src, dest);
	appendLineStringBuf(filelist, dest);
	source = source->next;
    }
    /* ... and icons */
    package = s->packages;
    while (package) {
	if (package->icon) {
	    sprintf(src, "%s/%s", getVar(RPMVAR_SOURCEDIR), package->icon);
	    sprintf(dest, "%s/%s", tempdir, source->source);
	    appendLineStringBuf(filelist, dest);
	    symlink(src, dest);
	}
	package = package->next;
    }

    /**************************************************/
    
    /* Now start packaging */
    if (!getEntry(s->packages->header, RPMTAG_VERSION, NULL,
		  (void *) &version, NULL)) {
	error(RPMERR_BADSPEC, "No version field");
	return RPMERR_BADSPEC;
    }
    if (!getEntry(s->packages->header, RPMTAG_RELEASE, NULL,
		  (void *) &release, NULL)) {
	error(RPMERR_BADSPEC, "No release field");
	return RPMERR_BADSPEC;
    }

    sprintf(filename, "%s-%s-%s.src.rpm", s->name, version, release);
    fd = open(filename, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (writeMagic(s, fd, s->name, RPMLEAD_SOURCE)) {
	return 1;
    }

    outHeader = copyHeader(s->packages->header);
    os = getArchNum();
    arch = getArchNum();
    addEntry(outHeader, RPMTAG_OS, INT8_TYPE, &os, 1);
    addEntry(outHeader, RPMTAG_ARCH, INT8_TYPE, &arch, 1);
    addEntry(outHeader, RPMTAG_BUILDTIME, INT32_TYPE, &buildtime, 1);
    addEntry(outHeader, RPMTAG_BUILDHOST, STRING_TYPE, buildHost(), 1);
    /* XXX - need: distribution, vendor, release */

    if (process_filelist(outHeader, filelist, &size, RPMLEAD_SOURCE)) {
	return 1;
    }
    
    addEntry(outHeader, RPMTAG_SIZE, INT32_TYPE, &size, 1);

    writeHeader(fd, outHeader);

    /* Now do the cpio | gzip thing */
    if (cpio_gzip(outHeader, fd, tempdir)) {
	return 1;
    }
    
    close(fd);
    freeHeader(outHeader);

    /**************************************************/

    /* Now clean up */

    freeStringBuf(filelist);
    
    source = s->sources;
    while (source) {
	sprintf(dest, "%s/%s", tempdir, source->source);
	unlink(dest);
	source = source->next;
    }
    package = s->packages;
    while (package) {
	if (package->icon) {
	    sprintf(dest, "%s/%s", tempdir, source->source);
	    unlink(dest);
	}
	package = package->next;
    }
    rmdir(tempdir);
    
    return 0;
}
