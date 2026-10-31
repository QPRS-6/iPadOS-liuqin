/*
 * liuqin-backboardd: start the SpringBoard with our library inserted.
 *
 * iOS launchd ignores a job's EnvironmentVariables, so DYLD_INSERT_LIBRARIES
 * never reaches backboardd that way (the kernel can see it: the exec carries 7
 * variables and none of them starts with DYLD_).  launchd does start whatever
 * ProgramArguments names, so it starts this program instead, which sets the
 * environment and execs the real daemon in place - same pid, same argv[0], same
 * MachServices, and dyld now sees the variable.
 *
 * Build: tools/build-inject.sh (arm64, iOS 16).
 */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SPRINGBOARD      "/System/Library/CoreServices/SpringBoard.app/SpringBoard.real"
/* The library travels inside this binary (see host/liuqin_dylib_blob.py): an
 * image flash that leaves parts stale then cannot give us an old build.  It is
 * written to a writable directory and inserted from there. */
/* Where the embedded library is written: the first of these the user can
 * create, mobile's own caches first (it always exists and is mobile's). */
static const char *const libraryDirs[] = {
	"/private/var/mobile/Library/Caches",
	"/private/var/tmp",
	"/var/tmp",
	"/tmp",
	NULL,
};
static char libraryPath[256];
#define OUR_LIBRARY     ((const char *)libraryPath)
#include "liuqin_setup_dylib_blob.h"

extern char **environ;

static uint32_t
fnv(const unsigned char *p, size_t n)
{
	uint32_t h = 2166136261u;

	for (size_t i = 0; i < n; i++) {
		h = (h ^ p[i]) * 16777619u;
	}
	return h;
}

static void
dump(const char *what, const unsigned char *p, size_t n)
{
	char line[3 * 16 + 1];
	size_t k = 0;

	for (size_t i = 0; i < n; i++) {
		k += (size_t)snprintf(line + k, sizeof(line) - k, "%02x", p[i]);
	}
	dprintf(2, "LIUQIN LAUNCHER: %s first bytes %s\n", what, line);
}

static bool
writeLibrary(void)
{
	size_t done;
	int fd, error;

	for (int i = 0; libraryDirs[i] != NULL; i++) {
		/* A name nobody has used before: this port's HFS driver fails (and marks
		 * the volume inconsistent) when it truncates an existing file. */
		snprintf(libraryPath, sizeof(libraryPath), "%s/libliuqinsetup-%d.dylib",
		    libraryDirs[i], (int)getpid());
		fd = open(libraryPath, O_WRONLY | O_CREAT | O_EXCL, 0755);
		if (fd < 0) {
			dprintf(2, "LIUQIN LAUNCHER: cannot create %s (errno %d)\n",
			    libraryPath, errno);
			continue;
		}
		done = 0;
		error = 0;
		while (done < sizeof(liuqin_dylib)) {
			ssize_t n = write(fd, liuqin_dylib + done, sizeof(liuqin_dylib) - done);

			if (n <= 0) {
				error = errno;
				break;
			}
			done += (size_t)n;
		}
		if (error == 0) {
			fsync(fd);
		}
		close(fd);
		if (error == 0) {
			unsigned char check[4096];
			FILE *again = fopen(libraryPath, "rb");
			size_t got = again ? fread(check, 1, sizeof(check), again) : 0;

			if (again) {
				fclose(again);
			}
			if (got == sizeof(check) && fnv(check, got) == fnv(liuqin_dylib, got)) {
				return true;
			}
			dprintf(2, "LIUQIN LAUNCHER: %s reads back different (got %zu bytes)\n",
			    libraryPath, got);
			error = EIO;
		}
		dprintf(2, "LIUQIN LAUNCHER: write to %s failed (errno %d)\n",
		    libraryPath, error);
	}
	return false;
}

int
main(int argc, char *argv[])
{
	char **child_argv;

	/* First thing: say we were started at all. */
	dprintf(2, "LIUQIN LAUNCHER: started (uid %d pid %d ppid %d)\n", (int)getuid(),
	    (int)getpid(), (int)getppid());
	int i;

	child_argv = calloc((size_t)argc + 1, sizeof(char *));
	if (child_argv == NULL) {
		return 127;
	}
	for (i = 0; i < argc; i++) {
		child_argv[i] = argv[i];
	}
	child_argv[0] = (char *)SPRINGBOARD;
	child_argv[argc] = NULL;

	dprintf(2, "LIUQIN LAUNCHER: uid %d, inserting %s into %s\n", (int)getuid(),
	    OUR_LIBRARY, SPRINGBOARD);
	if (!writeLibrary()) {
		goto run;
	}

	/* Say what the library file really is: the boot log then shows whether the
	 * write (and the build we embedded) is what we think it is. */
	{
		FILE *lib = fopen(OUR_LIBRARY, "rb");

		if (lib != NULL) {
			unsigned char buffer[4096];
			size_t got = fread(buffer, 1, sizeof(buffer), lib);
			uint32_t hash = 2166136261u;
			bool stub = false;
			long size;
			static const char needle[] = "libLiUqinHIDPrivate";

			for (size_t i = 0; i < got; i++) {
				hash = (hash ^ buffer[i]) * 16777619u;
			}
			stub = (got >= sizeof(needle) - 1) &&
			    memmem(buffer, got, needle, sizeof(needle) - 1) != NULL;
			fseek(lib, 0, SEEK_END);
			size = ftell(lib);
			fclose(lib);
			dump("embedded", liuqin_dylib, 16);
			dump("file    ", buffer, 16);
			dprintf(2, "LIUQIN LAUNCHER: embedded FNV %08x, file FNV %08x, file size %ld\n",
			    fnv(liuqin_dylib, 4096), hash, size);
			dprintf(2, "LIUQIN LAUNCHER: %s: %ld bytes, first-%zu FNV %08x, stub-reference %s\n",
			    OUR_LIBRARY, size, got, hash, stub ? "YES" : "no");
			if (stub) {
				dprintf(2, "LIUQIN LAUNCHER: not inserting a library that wants libLiUqinHIDPrivate\n");
				goto run;
			}
		}
	}
	/* Ask dyld directly why it might refuse it: for an inserted library that
	 * fails to load, dyld only says so when "allow insert failures" is off, so
	 * the reason is asked for here instead. */
	{
		void *handle = dlopen(OUR_LIBRARY, RTLD_NOW);

		if (handle == NULL) {
			dprintf(2, "LIUQIN LAUNCHER: dlopen(%s) failed: %s\n", OUR_LIBRARY, dlerror());
		} else {
			dprintf(2, "LIUQIN LAUNCHER: dlopen(%s) succeeded\n", OUR_LIBRARY);
			dlclose(handle);
		}
	}

	if (access(OUR_LIBRARY, R_OK) == 0) {
		setenv("DYLD_INSERT_LIBRARIES", OUR_LIBRARY, 1);
		setenv("DYLD_PRINT_LIBRARIES", "1", 1);
	} else {
		dprintf(2, "LIUQIN LAUNCHER: %s is missing, starting the SpringBoard alone\n",
		    OUR_LIBRARY);
	}

run:
	dprintf(2, "LIUQIN LAUNCHER: uid %d, inserting '%s' into %s\n", (int)getuid(),
	    OUR_LIBRARY, SPRINGBOARD);
	execv(SPRINGBOARD, child_argv);
	/*
	 * launchd starts a job with POSIX_SPAWN_SETEXEC, so this process *is* pid 1:
	 * exiting here kills init and panics the machine before the log reaches the
	 * disk.  Stay alive instead - a sleeping init is not a panic, and the log
	 * then says exactly what failed.
	 */
	dprintf(2, "LIUQIN LAUNCHER: execve(%s) failed, errno %d - staying alive as pid %d\n",
	    SPRINGBOARD, errno, (int)getpid());
	for (;;) {
		sleep(60);
	}
	/* Only reached if the exec failed: say so where the boot log can see it. */
	{
		extern int dprintf(int, const char *, ...);
		dprintf(2, "liuqin-backboardd: cannot exec %s\n", SPRINGBOARD);
	}
	return 127;
}
