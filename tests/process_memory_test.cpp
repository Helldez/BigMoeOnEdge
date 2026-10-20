#include "platform_io.h"

#include <mach/mach.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

int failures = 0;
task_vm_info_data_t reply{};
mach_msg_type_number_t reply_count = TASK_VM_INFO_REV1_COUNT;
kern_return_t reply_status = KERN_SUCCESS;

void check(bool condition, const char * name) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

bool same_sample(const bmoe::pio::ProcessMemory & a, const bmoe::pio::ProcessMemory & b) {
    return a.rss_bytes == b.rss_bytes && a.rss_anon_bytes == b.rss_anon_bytes && a.rss_file_bytes == b.rss_file_bytes &&
           a.swap_bytes == b.swap_bytes && a.process_footprint_known == b.process_footprint_known &&
           a.process_footprint_bytes == b.process_footprint_bytes &&
           a.process_headroom_known == b.process_headroom_known && a.process_headroom_bytes == b.process_headroom_bytes;
}

} // namespace

// This target compiles the real adapter with task_info renamed to this reply source. Truncating
// the kernel reply exercises revision handling without requiring an older running kernel.
extern "C" kern_return_t
bmoe_test_task_info(task_name_t, task_flavor_t flavor, task_info_t out, mach_msg_type_number_t * count) {
    check(flavor == TASK_VM_INFO, "adapter requests TASK_VM_INFO");
    if (flavor != TASK_VM_INFO) return KERN_INVALID_ARGUMENT;
    if (reply_status != KERN_SUCCESS) return reply_status;
    *count = std::min(*count, reply_count);
    std::memcpy(out, &reply, *count * sizeof(natural_t));
    return KERN_SUCCESS;
}

int main() {
    using bmoe::pio::process_memory;
    using bmoe::pio::ProcessMemory;

    reply.resident_size = 101;
    reply.internal = 102;
    reply.external = 103;
    reply.compressed = 104;
    reply.phys_footprint = 105;
    ProcessMemory sample;
    check(process_memory(&sample), "REV1 reply is accepted");
    check(sample.rss_bytes == 101 && sample.rss_anon_bytes == 102 && sample.rss_file_bytes == 103 &&
              sample.swap_bytes == 104,
          "legacy counters retain their distinct kernel values");
    check(sample.process_footprint_known && sample.process_footprint_bytes == 105,
          "REV1 footprint is read directly rather than reconstructed from RSS and compression");
    check(!sample.process_headroom_known && sample.process_headroom_bytes == 0, "macOS headroom remains unknown");

    reply.phys_footprint = 0;
    check(process_memory(&sample), "known-zero reply is accepted");
    check(sample.process_footprint_known && sample.process_footprint_bytes == 0,
          "reported zero footprint remains a known measurement");

    sample.process_footprint_bytes = 99;
    sample.process_headroom_known = true;
    sample.process_headroom_bytes = 98;
    reply_count = TASK_VM_INFO_REV0_COUNT;
    check(process_memory(&sample), "REV0 reply still supplies legacy counters");
    check(!sample.process_footprint_known && sample.process_footprint_bytes == 0 && !sample.process_headroom_known &&
              sample.process_headroom_bytes == 0,
          "an older reply clears stale known flags and optional counters");
    check(sample.rss_bytes == 101 && sample.rss_anon_bytes == 102 && sample.rss_file_bytes == 103 &&
              sample.swap_bytes == 104,
          "REV0 preserves all reported legacy counters");

    sample.rss_bytes = 201;
    sample.rss_anon_bytes = 202;
    sample.rss_file_bytes = 203;
    sample.swap_bytes = 204;
    sample.process_footprint_known = true;
    sample.process_footprint_bytes = 205;
    sample.process_headroom_known = true;
    sample.process_headroom_bytes = 206;
    const ProcessMemory before = sample;
    reply_count = TASK_VM_INFO_REV0_COUNT - 1;
    check(!process_memory(&sample), "a reply shorter than REV0 is rejected");
    check(same_sample(sample, before), "a short reply leaves the entire output untouched");

    reply_count = TASK_VM_INFO_COUNT;
    reply_status = KERN_FAILURE;
    check(!process_memory(&sample), "a task_info failure is reported");
    check(same_sample(sample, before), "a failed syscall leaves the entire output untouched");

    return failures ? 1 : 0;
}
