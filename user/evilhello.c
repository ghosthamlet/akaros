// evil hello world -- kernel pointer passed to kernel
// kernel should destroy user environment in response

#include <inc/lib.h>

void
umain(void)
{
	while(1);
	// try to print the kernel entry point as a string!  mua ha ha!
	sys_cputs((char*SAFE)TC(0xc0100020), 100);
}

