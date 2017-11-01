#include <unistd.h>
#include <sys/mman.h>
#include <sched.h>

internal u64
LockedAddAndReturnPreviousValue(u64 volatile *Value, u64 Addend)
{
    u64 Result = __sync_fetch_and_add(Value, Addend);
    return(Result);
}

internal s32
WorkerThread(void *arg)
{
    work_queue *Queue = (work_queue *)arg;
    while(RenderTile(Queue)) {};
    return(0);
}

internal u32
GetCPUCoreCount(void)
{
    u32 Result = sysconf(_SC_NPROCESSORS_ONLN);
    return(Result);
}

internal void
CreateWorkThread(void *Parameter)
{
    u32 stackSize = 1000000;
    void *stack = mmap(0, stackSize, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    u8 *stackTop = (u8 *)stack + stackSize;
    clone(WorkerThread, stackTop, 0, Parameter);
}
