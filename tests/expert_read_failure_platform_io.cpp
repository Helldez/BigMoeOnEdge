// Compile the production adapter with one syscall replaced (the pattern of
// startup_pin_platform_io.cpp): every positioned read goes through the test's switch, so a read can
// be made to fail on a real session over a real file. Nothing in the production library changes.
#include <sys/types.h>
#include <unistd.h>

ssize_t expert_read_test_pread(int fd, void * buf, size_t count, off_t off);

#define pread expert_read_test_pread
#include "../core/src/io/platform_io.cpp"
