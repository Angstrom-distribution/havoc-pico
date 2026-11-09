#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <pwd.h>

/* Child side of starting the user's login shell: the shell is $SHELL, else
 * the passwd entry's, else /bin/sh; argv[0] is "-" and its basename, which
 * is what makes a shell a login shell; the working directory is $HOME, else
 * the passwd home; HOME, SHELL, USER and LOGNAME are set where unset.
 * Returns only when the exec failed, with the shell tried in *prog. */
static void login_shell(const char **prog)
{
	struct passwd *pw = getpwuid(getuid());
	const char *shell = getenv("SHELL");
	const char *home = getenv("HOME");
	char arg0[PATH_MAX];
	const char *slash;

	if (!shell || !*shell || access(shell, X_OK) != 0)
		shell = pw && pw->pw_shell && *pw->pw_shell ?
			pw->pw_shell : "/bin/sh";
	if ((!home || !*home) && pw)
		home = pw->pw_dir;
	if (home && *home && chdir(home) == 0)
		setenv("HOME", home, 0);
	if (pw) {
		setenv("SHELL", pw->pw_shell, 0);
		setenv("USER", pw->pw_name, 0);
		setenv("LOGNAME", pw->pw_name, 0);
	}

	slash = strrchr(shell, '/');
	snprintf(arg0, sizeof(arg0), "-%s", slash ? slash + 1 : shell);
	*prog = shell;
	execl(shell, arg0, (char *) NULL);
}
