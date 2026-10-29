/* Fork children; in each, query sigaltstack before and after a jump. */
#define _DARWIN_C_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
static jmp_buf jb;
static sigjmp_buf sjb;
static void jump(int how) {
	if (how == 1) longjmp(jb, 1);
	if (how == 2) _longjmp(jb, 1);
	if (how == 3) siglongjmp(sjb, 1);
}
static int probe(int how, int fd) {
	stack_t a, b;
	sigaltstack(NULL, &a);
	if (how == 1) { if (setjmp(jb) == 0) jump(1); }
	else if (how == 2) { if (_setjmp(jb) == 0) jump(2); }
	else if (how == 3) { if (sigsetjmp(sjb, 0) == 0) jump(3); }
	sigaltstack(NULL, &b);
	char line[160];
	int n = snprintf(line, sizeof line, "before %d/%p/%zu after %d/%p/%zu\n",
	    a.ss_flags, a.ss_sp, (size_t)a.ss_size, b.ss_flags, b.ss_sp, (size_t)b.ss_size);
	write(fd, line, (size_t)n);
	return 0;
}
int main(int argc, char **argv) {
	int how = atoi(argv[1]), i;
	const char *names[] = { "none", "setjmp/longjmp", "_setjmp/_longjmp", "sigsetjmp(0)/siglongjmp" };
	int p[2]; pipe(p);
	printf("== %s\n", names[how]); fflush(stdout);
	for (i = 0; i < 100; i++) {
		pid_t c = fork();
		if (c == 0) { close(p[0]); probe(how, p[1]); exit(0); }
		int st; waitpid(c, &st, 0);
		if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { char m[64]; int n = snprintf(m, sizeof m, "child died status %#x\n", st); write(p[1], m, (size_t)n); }
	}
	close(p[1]);
	char buf[65536]; ssize_t r; size_t tot = 0;
	while ((r = read(p[0], buf + tot, sizeof buf - tot - 1)) > 0) tot += (size_t)r;
	buf[tot] = 0; fputs(buf, stdout);
	return 0;
}
