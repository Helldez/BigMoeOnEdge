// The defaults every front-end gives the compute backends (backend_env.h).

#include "backend_env.h"

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace bmoe::frontend {

namespace {
// The folder the backends keep what they compile between runs, inside the data folder.
fs::path backend_cache(const std::string & data_dir) {
    return fs::u8path(data_dir) / "backend-cache";
}
} // namespace

std::vector<BackendDefault> backend_defaults(const std::string & data_dir) {
    std::vector<BackendDefault> d;
    // A backend's own default is a temporary directory, which an app's process may not be able to
    // write, and the cache then disables itself. Named only where the folder exists: one that could not
    // be made (a data folder under a home the process cannot write) would disable it the same way, where
    // the backend's own default might still work.
    std::error_code ec;
    if (fs::is_directory(backend_cache(data_dir), ec))
        d.push_back({"OpenCL", "GGML_OPENCL_KERNEL_CACHE_DIR", backend_cache(data_dir).u8string(),
                     "compiled kernels kept between runs, in a folder every front-end can write"});
    d.insert(d.end(),
             {
                 // None for Hexagon: polling (GGML_HEXAGON_OPPOLL) logged a million error rows a load for under
                 // 1 s of a 16 s prefill, and its host buffer (GGML_HEXAGON_HOSTBUF, a debugging aid upstream)
                 // cost 7-13% of the decode and hung Gemma 4's load on the NPU (#512, #544; docs/backends.md).
                 // Read by ggml's CUDA backend, which is also its ROCm backend (HIP builds the same source). It
                 // enables the backend's generic entry point for pinning memory a process allocated itself
                 // (ggml_backend_register_host_buffer), which the engine asks for by that name whatever the
                 // backend; llama.cpp does not call it.
                 {"CUDA, ROCm", "GGML_CUDA_REGISTER_HOST", "1",
                  "the experts' host copy for a device cache pinned in place: 117 GB took 97 s to allocate pinned "
                  "on a rented 3090, where pinning ordinary memory took 6 s every 32 GB"},
             });
    return d;
}

void prepare_compute_backends(const std::string & data_dir) {
    std::error_code ec;
    fs::create_directories(backend_cache(data_dir), ec);
    for (const BackendDefault & d : backend_defaults(data_dir)) {
#if defined(_WIN32)
        if (!std::getenv(d.name)) _putenv_s(d.name, d.value.c_str());
#else
        setenv(d.name, d.value.c_str(), 0);
#endif
    }
}

} // namespace bmoe::frontend
