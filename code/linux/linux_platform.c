#include <time.h>
#include <sys/mman.h>

#include "../layer.h"
#include "linux_platform.h"

internal f32
GetTime()
{
	struct timespec time_spec_thing = {0};
	clock_gettime(CLOCK_REALTIME, &time_spec_thing);
	float seconds_elapsed =
		((float)time_spec_thing.tv_nsec / (float)1e9);
	return seconds_elapsed;
}

internal void *
RequestMem(u32 size)
{
	return mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
}

internal
void FreeMem(void *mem, u32 size)
{
    munmap(mem, size);
}
