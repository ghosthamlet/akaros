
#include <rstdio.h>
#include <parlib.h>

int main(int argc, char **argv, char **envp)
{
	#define FILENAME "/bin/hello"
	#if 0
	/* try some bad combos */
	int pid = sys_proc_create("garbagexxx");
	printf("Garbage pid result: %d\n", pid);

	error_t err = sys_proc_run(2342);
	printf("sys_proc_run(2342) error: %e\n", err);

	err = sys_proc_run(-1);
	cprintf("sys_proc_run(-1) error: %e\n", err);
	#endif

	#define NUM_KIDS 5
	int child_pid[NUM_KIDS];
	#if 0
	printf("U: attempting to create hello(s)\n");
	for (int i = 0; i < NUM_KIDS; i++)
		child_pid[i] = sys_proc_create("roslib_hello");

	for (int i = 0; i < NUM_KIDS; i++) {
		cprintf("U: attempting to run hello (pid: %d)\n", child_pid[i]);
		sys_proc_run(child_pid[i]);
	}
	#endif
	printf("U: attempting to create and run hello\n");
	child_pid[0] = sys_proc_create(FILENAME, strlen(FILENAME), 0, 0);
	if (child_pid[0] <= 0)
		perror("");
	else
		if (sys_proc_run(child_pid[0]) < 0)
			perror("");
	#if 0
	printf("U: attempting to create and run another hello\n");
	child_pid[1] = sys_proc_create(FILENAME, strlen(FILENAME), 0, 0);
	if (child_pid[1] <= 0)
		perror("");
	else
		if (sys_proc_run(child_pid[1]) < 0)
			perror("");
	#endif
	return 0;
}
