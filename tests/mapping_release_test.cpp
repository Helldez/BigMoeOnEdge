// release_file_mappings (core/src/io/mapping_release.h): the engine's answer to Windows serialising
// concurrent unbuffered reads on a file that has a live section. This test builds exactly the state
// llama.cpp leaves after load — a view and a section handle on a file, the file handle itself already
// closed — asks the module to release it, and checks both what it must do (drop the view, close the
// section) and what it must leave behind on purpose (a reservation where the view was, an inert
// handle in the section's slot) so that the model's later teardown of "its" mapping is harmless.
// No model, no llama.cpp: a scratch file is enough. On POSIX the same release is a munmap with an
// anonymous placeholder left in the range, checked the same way.
#include "mapping_release.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                       \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

#if defined(_WIN32)

namespace {

std::string scratch_path() {
    char dir[MAX_PATH];
    const DWORD n = GetTempPathA(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) return {};
    char path[MAX_PATH];
    if (!GetTempFileNameA(dir, "bmr", 0, path)) return {};
    return path;
}

bool fill(const std::string & path, size_t bytes) {
    FILE * f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::vector<char> chunk(1 << 16, 'x');
    for (size_t done = 0; done < bytes; done += chunk.size())
        if (std::fwrite(chunk.data(), 1, chunk.size(), f) != chunk.size()) {
            std::fclose(f);
            return false;
        }
    std::fclose(f);
    return true;
}

// The post-load state of llama_mmap on Windows: view + section handle alive, file handle closed.
struct LlamaLikeMapping {
    HANDLE section = nullptr;
    void * view = nullptr;
    bool open(const std::string & path) {
        HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        section = CreateFileMappingA(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
        CloseHandle(h);
        if (!section) return false;
        view = MapViewOfFile(section, FILE_MAP_READ, 0, 0, 0);
        return view != nullptr;
    }
};

DWORD state_at(void * p) {
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi) ? mbi.State : 0;
}

} // namespace

int main() {
    const size_t size = 8u << 20;
    const std::string path = scratch_path();
    CHECK(!path.empty());
    CHECK(fill(path, size));

    LlamaLikeMapping m;
    CHECK(m.open(path));
    CHECK(state_at(m.view) == MEM_COMMIT);
    CHECK(((const volatile char *) m.view)[0] == 'x'); // the view is real before we start

    bmoe::pio::MappingPlaceholders ph;
    bmoe::pio::MappingReleaseReport r = bmoe::pio::release_file_mappings({path}, &ph);
    CHECK(r.supported);
    CHECK(r.error.empty());
    CHECK(r.views_unmapped == 1);
    CHECK(r.bytes == size);
    CHECK(r.sections_closed == 1);
    CHECK(r.plugs_missed == 0);
    CHECK(!ph.empty());

    // The view's range is now a reservation, not free and not mapped: nothing can be allocated
    // there, so the model's late UnmapViewOfFile on this base finds no live allocation.
    CHECK(state_at(m.view) == MEM_RESERVE);

    // The section's handle value is now an inert event: waiting on it times out instead of failing
    // as it would on a section, and the model's late CloseHandle will close this plug, nothing else.
    CHECK(WaitForSingleObject(m.section, 0) == WAIT_TIMEOUT);

    // What llama_mmap's destructor does, in its order: both must be harmless.
    CHECK(UnmapViewOfFile(m.view) == FALSE);
    CHECK(CloseHandle(m.section) == TRUE);

    // Idempotent: nothing of this file is left to find.
    bmoe::pio::MappingPlaceholders ph2;
    r = bmoe::pio::release_file_mappings({path}, &ph2);
    CHECK(r.supported);
    CHECK(r.views_unmapped == 0);
    CHECK(r.sections_closed == 0);
    CHECK(ph2.empty());

    // Releasing the placeholders frees the range.
    ph.release();
    CHECK(ph.empty());
    CHECK(state_at(m.view) == MEM_FREE);

    DeleteFileA(path.c_str());
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("mapping_release: ok\n");
    return 0;
}

#else

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

int main() {
    // A file this process has mapped, the way llama.cpp maps a gguf: MAP_PRIVATE, read-only.
    char path[] = "/tmp/bmoe-mapping-release-XXXXXX";
    const int fd = mkstemp(path);
    CHECK(fd >= 0);
    const size_t size = 8u << 20;
    CHECK(ftruncate(fd, (off_t) size) == 0);
    void * view = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    CHECK(view != MAP_FAILED);
    close(fd);

    bmoe::pio::MappingPlaceholders ph;
    bmoe::pio::MappingReleaseReport r = bmoe::pio::release_file_mappings({path}, &ph);
    CHECK(r.supported);
    CHECK(r.error.empty());
    CHECK(r.views_unmapped == 1);
    CHECK(r.bytes == size);
    CHECK(r.sections_closed == 0);
    CHECK(!ph.empty());

    // The range is held by an inaccessible placeholder: mapping something else there with
    // MAP_FIXED_NOREPLACE must fail, and the model's own late munmap of the range is harmless.
#ifdef MAP_FIXED_NOREPLACE
    void * clash = mmap(view, size, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    CHECK(clash == MAP_FAILED);
#endif
    CHECK(munmap(view, size) == 0);

    // Idempotent, and forgetting the placeholders touches nothing.
    bmoe::pio::MappingPlaceholders ph2;
    r = bmoe::pio::release_file_mappings({path}, &ph2);
    CHECK(r.supported && r.views_unmapped == 0);
    ph.release();
    CHECK(ph.empty());

    unlink(path);
    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("mapping_release: ok\n");
    return 0;
}

#endif
