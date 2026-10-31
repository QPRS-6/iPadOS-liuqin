/*
 * liuqin-kick: ask launchd about the window server's job, and start it if it is
 * there.  Run as a root launchd job; launchctl inherits our stdout/stderr, which
 * the kernel's /dev/null tee copies to the console (and the kernel's log ring).
 */
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

static void
exec_tool(const char *tool, char *const argv[])
{
	pid_t pid = fork();
	int status = 0;

	if (pid == 0) {
		execv(tool, argv);
		dprintf(2, "LIUQIN KICK: cannot run %s\n", tool);
		_exit(127);
	}
	if (pid > 0) {
		(void)waitpid(pid, &status, 0);
		dprintf(2, "LIUQIN KICK: %s -> status 0x%x\n", tool, status);
	}
}

int
main(void)
{
	char *list[] = { "/bin/launchctl", "list", NULL };
	char *print[] = { "/bin/launchctl", "print", "system/com.apple.backboardd", NULL };
	char *kick[] = { "/bin/launchctl", "kickstart", "-k", "system/com.apple.backboardd", NULL };

	dprintf(2, "LIUQIN KICK: alive (uid %d pid %d)\n", (int)getuid(), (int)getpid());
	exec_tool("/bin/launchctl", list);
	exec_tool("/bin/launchctl", print);
	exec_tool("/bin/launchctl", kick);
	exec_tool("/bin/launchctl", list);
	for (;;) {
		sleep(600);
	}
}
