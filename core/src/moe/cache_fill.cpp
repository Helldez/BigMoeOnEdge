#include "cache_fill.h"

#include "../io/file_reader.h"
#include "gguf_offsets.h"

#include "ggml.h"
#include "llama.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bmoe {

struct CacheFill::Impl {
    GgufOffsets off;
    std::vector<std::unique_ptr<FileReader>> readers;                       // one per shard
    std::unordered_map<const ggml_tensor *, std::pair<int, uint64_t>> base; // tensor -> shard, data offset

    std::vector<std::thread> lanes;
    std::mutex m;
    std::condition_variable cv, done_cv;
    uint64_t gen = 0; // bumped per batch of jobs
    int active = 0;   // lanes still on the current batch
    bool stop = false;

    std::vector<llama_moe_cache_fill> jobs; // owned: an async batch outlives the call that started it
    std::vector<std::pair<int, uint64_t>> where;
    std::atomic<size_t> next{0};
    bool batch_async = false; // the current batch was started in the background
    int async_delay_us = 0;   // debug: how long a background batch waits before it reads

    void work(int lane) {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [&] { return stop || gen != seen; });
                if (stop) {
                    return;
                }
                seen = gen;
            }
            if (batch_async && async_delay_us > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(async_delay_us));
            }
            for (size_t i; (i = next.fetch_add(1)) < jobs.size();) {
                const llama_moe_cache_fill & f = jobs[i];
                const uint64_t size = f.w->nb[2];
                if (readers[where[i].first]->read(lane, f.dst, where[i].second + (uint64_t) f.expert * size, size) <
                    0) {
                    // a slot left half-written would compute garbage: there is no answer to give
                    fprintf(stderr, "bmoe: cache fill: read failed for %s expert %d\n", ggml_get_name(f.w), f.expert);
                    abort();
                }
            }
            std::lock_guard<std::mutex> lk(m);
            if (--active == 0) {
                done_cv.notify_all();
            }
        }
    }

    void wait_idle() {
        std::unique_lock<std::mutex> lk(m);
        done_cv.wait(lk, [&] { return active == 0; });
    }

    bool locate(const ggml_tensor * w, std::pair<int, uint64_t> & out) {
        auto it = base.find(w);
        if (it == base.end()) {
            const std::string name = ggml_get_name(w);
            const auto o = off.off_by_name.find(name);
            const auto f = off.file_by_name.find(name);
            if (o == off.off_by_name.end() || f == off.file_by_name.end()) {
                return false;
            }
            it = base.emplace(w, std::make_pair(f->second, o->second)).first;
        }
        out = it->second;
        return true;
    }

    void run(const llama_moe_cache_fill * f, size_t n, bool async) {
        wait_idle(); // the fills started before land first, in order
        if (n == 0) {
            return;
        }
        jobs.assign(f, f + n);
        where.resize(n);
        for (size_t i = 0; i < n; ++i) {
            if (!locate(jobs[i].w, where[i])) {
                fprintf(stderr, "bmoe: cache fill: no file offset for %s\n", ggml_get_name(jobs[i].w));
                abort();
            }
        }
        if (async && async_delay_us > 0) {
            // debug: the slot holds no expert until its fill lands (all ones: NaN in a float, garbage in a
            // block), so a lookup that reads it early cannot pass for a right one
            for (const llama_moe_cache_fill & j : jobs) {
                memset(j.dst, 0xff, j.w->nb[2]);
            }
        }
        {
            std::lock_guard<std::mutex> lk(m);
            next = 0;
            batch_async = async;
            active = (int) lanes.size();
            ++gen;
        }
        cv.notify_all();
        if (!async) {
            wait_idle();
        }
    }

    static void cb(void * ud, const llama_moe_cache_fill * f, size_t n, bool async) {
        static_cast<Impl *>(ud)->run(f, n, async);
    }

    ~Impl() {
        {
            std::lock_guard<std::mutex> lk(m);
            stop = true;
        }
        cv.notify_all();
        for (auto & t : lanes) {
            t.join();
        }
    }
};

CacheFill::CacheFill(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
CacheFill::~CacheFill() = default;

std::unique_ptr<CacheFill>
CacheFill::install(llama_context * ctx, const std::string & model_path, int lanes, int async_delay_us) {
    if (ctx == nullptr || lanes <= 0) {
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->async_delay_us = async_delay_us;
    impl->off = read_gguf_offsets(model_path.c_str());
    if (impl->off.shard_paths.empty()) {
        return nullptr;
    }
    for (const auto & path : impl->off.shard_paths) {
        auto r = std::make_unique<FileReader>();
        if (!r->open(path, lanes, /*direct=*/true, /*max_read=*/8u << 20)) {
            return nullptr;
        }
        impl->readers.push_back(std::move(r));
    }
    for (int i = 0; i < lanes; ++i) {
        impl->lanes.emplace_back([p = impl.get(), i] { p->work(i); });
    }
    if (!llama_set_moe_cache_fill(ctx, &Impl::cb, impl.get())) {
        return nullptr;
    }
    return std::unique_ptr<CacheFill>(new CacheFill(std::move(impl)));
}

} // namespace bmoe
