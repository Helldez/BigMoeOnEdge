// Compile the production adapter with test-only syscall replacements. Providing all its symbols
// here keeps the adapter object in bmoe_core's static archive unselected; no library build flags or
// production test hooks change. Mapping and I/O stay real, only the locked-page allowance is fake.
#include <sys/mman.h>

int startup_test_mlock(const void * p, size_t size);
int startup_test_munlock(const void * p, size_t size);
void * startup_test_mmap(void * p, size_t size, int prot, int flags, int fd, off_t off);
int startup_test_munmap(void * p, size_t size);

#define mlock startup_test_mlock
#define munlock startup_test_munlock
#define mmap startup_test_mmap
#define munmap startup_test_munmap
#include "../core/src/io/platform_io.cpp"
