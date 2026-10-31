/*
 * liuqin-probe: does this kernel let launchd run a binary of ours?
 *
 * The point is not what it does but that it runs at all: iOS expects every
 * daemon to carry an Apple signature, and the window server's replacement in
 * the touch port needs a library of ours loaded into it, which in turn needs
 * code of ours to execute.  This program says so on stderr (kept by the devnull
 * tee, boot-arg liuqinnull=...liuqin-probe) and leaves a file behind that the
 * kernel's own probe can report on the next boot.
 *
 * Build: tools/build-inject.sh (arm64, iOS 16).
 */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int
main(void)
{
	static const char mark[] = "/private/var/tmp/liuqin-probe-ok";
	int fd;

	dprintf(2, "LIUQIN PROBE: alive (uid %d pid %d)\n", (int)getuid(), (int)getpid());
	if ((fd = open(mark, O_WRONLY | O_CREAT | O_TRUNC, 0644)) >= 0) {
		dprintf(fd, "liuqin-probe ran as uid %d\n", (int)getuid());
		close(fd);
		dprintf(2, "LIUQIN PROBE: wrote %s\n", mark);
	} else {
		dprintf(2, "LIUQIN PROBE: cannot write %s\n", mark);
	}
	return 0;
}
