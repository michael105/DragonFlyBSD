/*
 * Copyright (c) 2019 The DragonFly Project.  All rights reserved.
 *
 * This code is derived from software contributed to The DragonFly Project
 * by Matthew Dillon <dillon@backplane.com>
 *
 * This code uses concepts and configuration based on 'synth', by
 * John R. Marino <draco@marino.st>, which was written in ada.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "dsynth.h"

static void domount(worker_t *work, int type,
			const char *spath, const char *dpath,
			const char *discretefmt);
static void dounmount(worker_t *work, const char *rpath);
static void makeDiscreteCopies(const char *spath, const char *discretefmt);

/*
 * Called by the frontend to create a template which will be cpdup'd
 * into fresh workers.
 *
 * Template must have been previously destroyed.  Errors are fatal
 */
int
DoCreateTemplate(int force)
{
	struct stat st;
	char *goodbuf;
	char *buf;
	int rc;
	int fd;

	rc = 0;
	asprintf(&goodbuf, "%s/.template.good", BuildBase);

	/*
	 * Conditionally create the template and discrete copies of certain
	 * directories if we think we are missing things.
	 */
	if (force == 0) {
		asprintf(&buf, "%s/Template", BuildBase);
		if (stat(buf, &st) < 0)
			force = 1;
		free(buf);

		if (stat(goodbuf, &st) < 0)
			force = 1;
	}

	dlog(DLOG_ALL, "Check Template: %s\n",
	     (force ? "Must-Create" : "Good"));

	/*
	 * Create the template
	 */
	if (force) {
		remove(goodbuf);	/* ignore exit code */

		rc = 0;
		asprintf(&buf, "%s/mktemplate %s %s/Template",
			 SCRIPTPATH(SCRIPTDIR), SystemPath, BuildBase);
		rc = system(buf);
		if (rc)
			dfatal("Command failed: %s\n", buf);
		dlog(DLOG_ALL | DLOG_FILTER,
		     "Template - rc=%d running %s\n", rc, buf);
		free(buf);

		/*
		 * Make discrete copies of certain extremely heavily used
		 * but small directories.
		 */
		if (force) {
			makeDiscreteCopies("$/bin", "/bin.%03d");
			makeDiscreteCopies("$/lib", "/lib.%03d");
			makeDiscreteCopies("$/libexec", "/libexec.%03d");
			makeDiscreteCopies("$/usr/bin", "/usr.bin.%03d");
		}

		/*
		 * Mark the template good... ah, do a sync() to really
		 * be sure that it can't get corrupted.
		 */
		sync();
		fd = open(goodbuf, O_RDWR|O_CREAT|O_TRUNC, 0644);
		dassert_errno(fd >= 0, "could not create %s", goodbuf);
		close(fd);

		dlog(DLOG_ALL | DLOG_FILTER, "Template - done\n");
	}
	free(goodbuf);

	return force;
}

void
DoDestroyTemplate(void)
{
	struct stat st;
	char *path;
	char *buf;
	int rc;

	/*
	 * NOTE: rm -rf safety, use a fixed name 'Template' to ensure we
	 *	 do not accidently blow something up.
	 */
	asprintf(&path, "%s/Template", BuildBase);
	if (stat(path, &st) == 0) {
		asprintf(&buf, "chflags -R noschg %s; /bin/rm -rf %s",
			 path, path);
		rc = system(buf);
		if (rc)
			dfatal("Command failed: %s (ignored)\n", buf);
		free(buf);
	}
	free(path);
}

/*
 * Called by the worker support thread to install a new worker
 * filesystem topology.
 */
void
DoWorkerMounts(worker_t *work)
{
	char *buf;
	int rc;

	/*
	 * Generate required mounts, domount() will mkdir() the target
	 * directory if necessary and prefix spath with SystemPath if
	 * it starts with $/
	 */
	domount(work, TMPFS_RW, "dummy", "", NULL);
	asprintf(&buf, "%s/usr", work->basedir);
	if (mkdir(buf, 0755) != 0) {
		fprintf(stderr, "Command failed: mkdir %s\n", buf);
		++work->mount_error;
	}
	domount(work, NULLFS_RO, "$/boot", "/boot", NULL);
	domount(work, TMPFS_RW,  "dummy", "/boot/modules.local", NULL);
	domount(work, DEVFS_RW,  "dummy", "/dev", NULL);
	domount(work, PROCFS_RO, "dummy", "/proc", NULL);
	domount(work, NULLFS_RO, "$/bin", "/bin", "/bin.%03d");
	domount(work, NULLFS_RO, "$/sbin", "/sbin", NULL);
	domount(work, NULLFS_RO, "$/lib", "/lib", "/lib.%03d");
	domount(work, NULLFS_RO, "$/libexec", "/libexec", "/libexec.%03d");
	domount(work, NULLFS_RO, "$/usr/bin", "/usr/bin", "/usr.bin.%03d");
	domount(work, NULLFS_RO, "$/usr/include", "/usr/include", NULL);
	domount(work, NULLFS_RO, "$/usr/lib", "/usr/lib", NULL);
	domount(work, NULLFS_RO, "$/usr/libdata", "/usr/libdata", NULL);
	domount(work, NULLFS_RO, "$/usr/libexec", "/usr/libexec", NULL);
	domount(work, NULLFS_RO, "$/usr/sbin", "/usr/sbin", NULL);
	domount(work, NULLFS_RO, "$/usr/share", "/usr/share", NULL);
	domount(work, TMPFS_RW,  "dummy", "/usr/local", NULL);
	domount(work, NULLFS_RO, "$/usr/games", "/usr/games", NULL);
	if (UseUsrSrc)
		domount(work, NULLFS_RO, "$/usr/src", "/usr/src", NULL);
	domount(work, NULLFS_RO, DPortsPath, "/xports", NULL);
	domount(work, NULLFS_RW, OptionsPath, "/options", NULL);
	domount(work, NULLFS_RW, PackagesPath, "/packages", NULL);
	domount(work, NULLFS_RW, DistFilesPath, "/distfiles", NULL);
	domount(work, TMPFS_RW_BIG, "dummy", "/construction", NULL);
	if (UseCCache)
		domount(work, NULLFS_RW, CCachePath, "/ccache", NULL);

	/*
	 * NOTE: Uses blah/. to prevent cp from creating 'Template' under
	 *	 work->basedir.  We want to start with the content.
	 */
	asprintf(&buf, "cp -Rp %s/Template/. %s", BuildBase, work->basedir);
	rc = system(buf);
	if (rc) {
		fprintf(stderr, "Command failed: %s\n", buf);
		++work->accum_error;
		snprintf(work->status, sizeof(work->status),
			 "Template copy failed");
	}
	free(buf);
}

/*
 * Called by the worker support thread to remove a worker
 * filesystem topology.
 *
 * NOTE: No need to conditionalize UseUsrSrc, it doesn't hurt to
 *	 issue the umount() if it isn't mounted and it ensures that
 *	 everything is unmounted properly on cleanup if the state
 *	 changes.
 */
void
DoWorkerUnmounts(worker_t *work)
{
	int retries;

	work->mount_error = 0;
	for (retries = 0; retries < 10; ++retries) {
		dounmount(work, "/proc");
		dounmount(work, "/dev");
		dounmount(work, "/usr/src");
		dounmount(work, "/usr/games");
		dounmount(work, "/boot/modules.local");
		dounmount(work, "/boot");
		dounmount(work, "/usr/local");
		dounmount(work, "/construction");
		dounmount(work, "/ccache");	/* in case of config change */
		dounmount(work, "/distfiles");
		dounmount(work, "/packages");
		dounmount(work, "/options");
		dounmount(work, "/xports");
		dounmount(work, "/usr/share");
		dounmount(work, "/usr/sbin");
		dounmount(work, "/usr/libexec");
		dounmount(work, "/usr/libdata");
		dounmount(work, "/usr/lib");
		dounmount(work, "/usr/include");
		dounmount(work, "/usr/bin");
		dounmount(work, "/libexec");
		dounmount(work, "/lib");
		dounmount(work, "/sbin");
		dounmount(work, "/bin");
		dounmount(work, "");
		if (work->mount_error == 0)
			break;
		sleep(5);
		work->mount_error = 0;
	}
	if (work->mount_error) {
		++work->accum_error;
		snprintf(work->status, sizeof(work->status),
			 "Unable to unmount slot");
	}
}

static
void
domount(worker_t *work, int type, const char *spath, const char *dpath,
	const char *discretefmt)
{
	const char *prog;
	const char *sbase;
	const char *rwstr;
	const char *optstr;
	struct stat st;
	char *buf;
	char *tmp;
	int rc;

	/*
	 * Make target directory if necessary.  This must occur in-order
	 * since directories may have to be created under prior mounts
	 * in the sequence.
	 */
	asprintf(&buf, "%s%s", work->basedir, dpath);
	if (stat(buf, &st) != 0) {
		if (mkdir(buf, 0755) != 0) {
			fprintf(stderr, "Command failed: mkdir %s\n", buf);
			++work->mount_error;
		}
	}
	free(buf);

	/*
	 * Setup for mount arguments
	 */
	rwstr = (type & MOUNT_TYPE_RW) ? "rw" : "ro";
	optstr = "";

	switch(type & MOUNT_TYPE_MASK) {
	case MOUNT_TYPE_TMPFS:
		prog = MOUNT_TMPFS_BINARY;
		if (type & MOUNT_TYPE_BIG)
			optstr = " -s 64g";
		else
			optstr = " -s 16g";
		break;
	case MOUNT_TYPE_NULLFS:
		prog = MOUNT_NULLFS_BINARY;
		break;
	case MOUNT_TYPE_DEVFS:
		prog = MOUNT_DEVFS_BINARY;
		break;
	case MOUNT_TYPE_PROCFS:
		prog = MOUNT_PROCFS_BINARY;
		break;
	default:
		dfatal("Illegal mount type: %08x", type);
		/* NOT REACHED */
		prog = "/bin/hell";
		break;
	}

	/*
	 * Prefix spath
	 */
	if (discretefmt) {
		sbase = BuildBase;
		asprintf(&tmp, discretefmt, work->index);
		spath = tmp;
	} else {
		if (spath[0] == '$') {
			++spath;
			sbase = SystemPath;
			if (strcmp(sbase, "/") == 0)
				++sbase;
		} else {
			sbase = "";
		}
		tmp = NULL;
	}
	asprintf(&buf, "%s%s -o %s %s%s %s%s",
		 prog, optstr, rwstr,
		 sbase, spath, work->basedir, dpath);
	rc = system(buf);
	if (rc) {
		fprintf(stderr, "Command failed: %s\n", buf);
		++work->mount_error;
	}
	free(buf);
	if (tmp)
		free(tmp);
}

static
void
dounmount(worker_t *work, const char *rpath)
{
	char *buf;

	asprintf(&buf, "%s%s", work->basedir, rpath);
	if (unmount(buf, 0) < 0) {
		switch(errno) {
		case ENOENT:
		case EINVAL:
			break;
		default:
			fprintf(stderr, "Cannot umount %s (%s)\n",
				buf, strerror(errno));
			++work->mount_error;
			break;
		}
	}
	free(buf);
}

static
void
makeDiscreteCopies(const char *spath, const char *discretefmt)
{
	char *src;
	char *dst;
	char *buf;
	struct stat st;
	int i;
	int rc;

	for (i = 0; i < MaxWorkers; ++i) {
		if (spath[0] == '$') {
			if (strcmp(SystemPath, "/") == 0)
				asprintf(&src, "%s%s",
					 SystemPath + 1, spath + 1);
			else
				asprintf(&src, "%s%s",
					 SystemPath, spath + 1);
		} else {
			src = strdup(spath);
		}
		asprintf(&buf, discretefmt, i);
		asprintf(&dst, "%s%s", BuildBase, buf);
		free(buf);

		if (stat(dst, &st) < 0) {
			if (mkdir(dst, 0555) < 0) {
				dlog(DLOG_ALL, "Template - mkdir %s failed\n",
				     dst);
				dfatal_errno("Cannot mkdir %s:", dst);
			}
		}
		asprintf(&buf, "chflags -R noschg %s; "
			       "rm -rf %s; "
			       "cp -Rp %s/. %s",
			       dst, dst, src, dst);
		rc = system(buf);
		dlog(DLOG_ALL | DLOG_FILTER,
		     "Template - rc=%d running %s\n", rc, buf);
		if (rc)
			dfatal("Command failed: %s", buf);
		free(buf);
		free(src);
		free(dst);
	}
}
