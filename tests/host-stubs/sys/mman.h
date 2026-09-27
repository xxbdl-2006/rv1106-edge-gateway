#ifndef _STUB_MMAN_H
#define _STUB_MMAN_H
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_FAILED ((void *)-1)
void *mmap(void *, unsigned long, int, int, int, long);
int munmap(void *, unsigned long);
#endif
