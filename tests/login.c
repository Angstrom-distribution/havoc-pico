/* Starts the login shell of login.h on a pty and checks what it sees.
 * Needs no compositor: cc -I. -o login tests/login.c -lutil && ./login */
#include <sys/wait.h>
#include <errno.h>
#include "../ptyutil.h"
#include "../login.h"

static int failures;

/* run login_shell in a child with the given environment, return what the
 * shell printed for "$0 $PWD $USER" */
static void run(const char *shell, const char *home, char *out, size_t len)
{
	int fd, n, total = 0;
	pid_t pid = forkpty(&fd, NULL, NULL, NULL);

	if (pid == 0) {
		const char *prog;
		if (shell)
			setenv("SHELL", shell, 1);
		else
			unsetenv("SHELL");
		if (home)
			setenv("HOME", home, 1);
		else
			unsetenv("HOME");
		unsetenv("USER");
		unsetenv("LOGNAME");
		login_shell(&prog);
		_exit(127);
	}
	dprintf(fd, "printf '<%%s|%%s|%%s|%%s>\\n' \"$0\" \"$PWD\" \"$USER\" \"$LOGNAME\"; exit\n");
	while (total < (int) len - 1 &&
	       (n = read(fd, out + total, len - 1 - total)) > 0)
		total += n;
	out[total] = '\0';
	waitpid(pid, NULL, 0);
	close(fd);
}

static void expect(const char *what, const char *out, const char *want)
{
	if (strstr(out, want))
		printf("ok   %s\n", what);
	else {
		printf("FAIL %s: wanted %s in:\n%s\n", what, want, out);
		failures++;
	}
}

int main(void)
{
	char dir[] = "/tmp/havoc-login-XXXXXX", real[PATH_MAX];
	char out[4096], want[PATH_MAX + 64];
	struct passwd *pw = getpwuid(getuid());

	if (!mkdtemp(dir) || !realpath(dir, real) || !pw)
		return 77;

	snprintf(want, sizeof(want), "<-sh|%s|%s|%s>", real, pw->pw_name,
		 pw->pw_name);
	run("/bin/sh", real, out, sizeof(out));
	expect("login argv[0], $HOME as cwd, USER and LOGNAME", out, want);

	run("/nonexistent/shell", real, out, sizeof(out));
	expect("unusable $SHELL falls back to the passwd shell or /bin/sh",
	       out, real);

	run(NULL, "/nonexistent/home", out, sizeof(out));
	expect("HOME that cannot be entered leaves the cwd alone", out, "<-");

	rmdir(dir);
	return failures != 0;
}
