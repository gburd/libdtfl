#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
static void show(const char *who) {
	stack_t ss;
	if (sigaltstack(NULL, &ss) != 0) { perror("sigaltstack"); return; }
	printf("%s: flags=%d sp=%p size=%zu\n", who, ss.ss_flags, ss.ss_sp, (size_t)ss.ss_size);
	fflush(stdout);
}
int main(void) {
	int i, bad = 0;
	show("parent");
	for (i = 0; i < 200; i++) {
		pid_t p = fork();
		if (p == 0) {
			stack_t ss;
			sigaltstack(NULL, &ss);
			if (i < 3) show("child");
			_exit(ss.ss_flags != SS_DISABLE && ss.ss_size == 0 ? 1 : 0);
		}
		int st; waitpid(p, &st, 0);
		if (WIFEXITED(st) && WEXITSTATUS(st) == 1) bad++;
	}
	printf("children with flags!=SS_DISABLE and size 0: %d of 200\n", bad);
	return 0;
}
