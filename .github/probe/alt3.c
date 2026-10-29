/* setjmp/longjmp with a stack-local jmp_buf over dirtied stack: does
 * longjmp leave the thread marked on the alternate signal stack? */
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void __attribute__((noinline)) dirty(unsigned char v) {
	volatile unsigned char buf[8192];
	size_t i;
	for (i = 0; i < sizeof buf; i++) buf[i] = v;
}
static int __attribute__((noinline)) jump_local(int mode, unsigned *unwritten) {
	jmp_buf jb;
	sigjmp_buf sjb;
	stack_t ss;
	unsigned i;
	*unwritten = 0;
	if (mode == 0) {
		if (setjmp(jb) == 0) {
			for (i = 0; i < sizeof jb / sizeof(int); i++) if (((unsigned *)(void *)jb)[i] == 0xffffffffu) *unwritten |= 1u << (i % 32), printf("  unwritten jb word %u\n", i);
			longjmp(jb, 1);
		}
	} else if (mode == 1) {
		memset(jb, 0, sizeof jb);
		if (setjmp(jb) == 0) longjmp(jb, 1);
	} else {
		if (sigsetjmp(sjb, 0) == 0) siglongjmp(sjb, 1);
	}
	sigaltstack(NULL, &ss);
	return ss.ss_flags;
}
int main(void) {
	const char *mn[] = { "setjmp", "setjmp+memset", "sigsetjmp(0)" };
	int v, m;
	for (v = 0; v < 2; v++) for (m = 0; m < 3; m++) {
		unsigned u; int f;
		pid_t c;
		dirty(v ? 0xff : 0x00); f = jump_local(m, &u);
		printf("parent dirty=%s %-14s flags after=%d\n", v ? "ff" : "00", mn[m], f); fflush(stdout);
		c = fork();
		if (c == 0) { dirty(v ? 0xff : 0x00); f = jump_local(m, &u); printf("child  dirty=%s %-14s flags after=%d\n", v ? "ff" : "00", mn[m], f); fflush(stdout); _exit(0); }
		waitpid(c, NULL, 0);
	}
	return 0;
}
