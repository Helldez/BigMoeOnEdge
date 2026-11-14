// The defaults every front-end gives the compute backends (backend_env.h).

#include "backend_env.h"

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace bmoe::frontend {

std::vector<BackendDefault> backend_defaults(const std::string & data_dir) {
    return {
        // A backend's own default is a temporary directory, which an app's process may not be able to
        // write, and the cache then disables itself.
        {"OpenCL", "GGML_OPENCL_KERNEL_CACHE_DIR", (fs::u8path(data_dir) / "backend-cache").u8string(),
         "compiled kernels kept between runs, in a folder every front-end can write"},
        // The backend polls only while a DSP op is in flight, so neither costs anything until the
        // planner arms the NPU. Measured on a phone; the same backend on Windows on Snapdragon reads
        // them too.
        {"Hexagon", "GGML_HEXAGON_OPPOLL", "1", "polling halves the cost of each crossing to the DSP"},
        {"Hexagon", "GGML_HEXAGON_HOSTBUF", "1",
         "offers the host buffer type the prefill device moves the KV cache into"},
        // Read by ggml's CUDA backend, which is also its ROCm backend (HIP builds the same source). It
        // enables the backend's generic entry point for pinning memory a process allocated itself
        // (ggml_backend_register_host_buffer), which the engine asks for by that name whatever the
        // backend; llama.cpp does not call it.
        {"CUDA, ROCm", "GGML_CUDA_REGISTER_HOST", "1",
         "the experts' host copy for a device cache pinned in place: 117 GB took 97 s to allocate pinned "
         "on a rented 3090, where pinning ordinary memory took 6 s every 32 GB"},
    };
}

void prepare_compute_backends(const std::string & data_dir) {
    std::error_code ec;
    fs::create_directories(fs::u8path(data_dir) / "backend-cache", ec);
    for (const BackendDefault & d : backend_defaults(data_dir)) {
#if defined(_WIN32)
        if (!std::getenv(d.name)) _putenv_s(d.name, d.value.c_str());
#else
        setenv(d.name, d.value.c_str(), 0);
#endif
    }
}

} // namespace bmoe::frontend
