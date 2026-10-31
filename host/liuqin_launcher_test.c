/*
 * liuqin-launcher-test: does launchd start a program of ours at all?
 *
 * Replacement for the window server that only says so and exits: the answer
 * decides whether the real launcher (which sets DYLD_INSERT_LIBRARIES and
 * execs backboardd) can be delivered the same way - through the job entry in
 * /System/Library/xpc/launchd.plist.
 */
#include <stdio.h>
#include <unistd.h>

int
main(void)
{
	dprintf(2, "LIUQIN LAUNCHER: alive (uid %d pid %d) - the cache's ProgramArguments ran\n",
	    (int)getuid(), (int)getpid());
	sleep(1);
	return 0;
}
