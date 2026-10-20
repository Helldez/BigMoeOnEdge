#include "model_inspect.h"

#include "model_files.h"

#include "bmoe/probe.h"

#include <atomic>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace bmoe::server {

namespace {

// How much of a file holds its header. The first shard carries the vocabulary (ten megabytes on
// a large model); the others carry a few kilobytes of tensor names. Asked for in one range each,
// and once more, wider, when the first did not parse: a header past the first guess is rare and
// costs one more request, a guess sized for it every time costs every lookup.
constexpr uint64_t k_first_shard_bytes = 32ull << 20;
constexpr uint64_t k_first_shard_retry_bytes = 192ull << 20;
constexpr uint64_t k_other_shard_bytes = 4ull << 20;

std::string base_name(const std::string & path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

uint64_t file_size_or_zero(const fs::path & p) {
    std::error_code ec;
    const auto n = fs::file_size(p, ec);
    return ec ? 0 : (uint64_t) n;
}

bool fetch_head(HttpFetch & net,
                const std::string & url,
                uint64_t want,
                uint64_t file_bytes,
                const std::string & token,
                const fs::path & dst,
                std::string & error) {
    const uint64_t to = file_bytes > 0 && file_bytes - 1 < want - 1 ? file_bytes - 1 : want - 1;
    std::string body;
    if (!net.get(url, 0, to, token, body, error)) return false;
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    out.write(body.data(), (std::streamsize) body.size());
    if (!out) {
        error = "could not write " + dst.filename().u8string() + " in the scratch folder";
        return false;
    }
    return true;
}

std::atomic<unsigned> g_seq{0};

} // namespace

bool inspect_remote_model(HttpFetch & net,
                          const std::string & endpoint,
                          const HfRef & ref,
                          const HfModel & model,
                          const std::string & token,
                          const std::string & scratch_dir,
                          const std::string & models_dir,
                          const LoadedSession & loaded,
                          json & out,
                          std::string & error) {
    if (model.files.empty()) {
        error = "the model has no files";
        return false;
    }
    std::error_code ec;
    const fs::path dir = fs::u8path(scratch_dir) / ("inspect-" + std::to_string(g_seq.fetch_add(1)));
    fs::create_directories(dir, ec);
    if (ec) {
        error = "could not create the scratch folder: " + ec.message();
        return false;
    }
    struct Cleanup {
        fs::path d;
        ~Cleanup() {
            std::error_code e;
            fs::remove_all(d, e);
        }
    } cleanup{dir};

    // Every shard's header, as truncated files under their own names: the probe resolves the
    // rest of a split set from the first shard's name, as the load does.
    const fs::path first = dir / fs::u8path(base_name(model.files.front().path));
    for (size_t i = 0; i < model.files.size(); ++i) {
        const HfFile & f = model.files[i];
        if (!fetch_head(net, hf_resolve_url(endpoint, ref, f.path), i == 0 ? k_first_shard_bytes : k_other_shard_bytes,
                        f.bytes, token, dir / fs::u8path(base_name(f.path)), error))
            return false;
    }
    GgufHeaderFacts header = probe_gguf_header(first.u8string().c_str());
    if (!header.ok && model.files.front().bytes > k_first_shard_bytes) {
        if (!fetch_head(net, hf_resolve_url(endpoint, ref, model.files.front().path), k_first_shard_retry_bytes,
                        model.files.front().bytes, token, first, error))
            return false;
        header = probe_gguf_header(first.u8string().c_str());
    }
    const ModelProfile m = header.ok ? probe_model(first.u8string().c_str()) : ModelProfile{};

    // This machine: what the process may hold, and the disk the download would land on.
    const HardwareProfile hw = probe_hardware(nullptr);
    const fs::space_info space = fs::space(fs::u8path(models_dir), ec);
    const uint64_t disk_free = ec ? 0 : (uint64_t) space.available;
    uint64_t present = 0;
    for (const HfFile & f : model.files) {
        const fs::path p = fs::u8path(models_dir) / fs::u8path(base_name(f.path));
        if (f.bytes > 0 && file_size_or_zero(p) == f.bytes) present += f.bytes;
    }

    FitInputs in;
    in.file_bytes = model.bytes;
    in.present_bytes = present;
    in.disk_free = disk_free;
    in.dense_bytes = m.dense_bytes;
    in.token_cycle = m.token_cycle_bytes;
    in.budget = hw.residency_budget;
    in.readable = header.ok;
    in.is_moe = header.n_expert > 1;
    in.streamable = m.ok && m.is_moe;
    if (header.ok && !m.ok) in.refusal = m.error;
    const std::string role = file_role_name(file_role(header));
    if (header.ok) in.role = role;
    in.memory_total = hw.memory_total;
    in.session_model = loaded.model;
    in.session_held = loaded.held_bytes;
    const FitVerdict v = fit_verdict(in);

    out = json{
        {"repo", ref.repo},
        {"revision", ref.revision},
        {"name", model.name},
        {"quant", model.quant},
        {"bytes", model.bytes},
        {"files", model.files.size()},
        {"complete", model.complete()},
        {"model", json{{"readable", header.ok},
                       {"arch", header.arch},
                       {"role", role},
                       {"type", header.type},
                       {"block_count", header.n_block},
                       {"is_moe", in.is_moe},
                       {"n_expert", header.n_expert},
                       {"n_expert_used", header.n_expert_used},
                       {"n_layer", m.n_layer},
                       {"n_moe_layer", m.n_moe_layer},
                       {"n_embd", m.n_embd},
                       {"dense_bytes", m.dense_bytes},
                       {"expert_bytes", m.expert_bytes},
                       {"token_cycle_bytes", m.token_cycle_bytes},
                       {"largest_dense_tensor", m.largest_dense_tensor},
                       {"has_mtp", m.has_mtp},
                       {"streamable", in.streamable},
                       {"refusal", in.refusal}}},
        {"machine", json{{"residency_budget", hw.residency_budget},
                         {"budget_if_loaded", v.budget},
                         {"loaded_model", loaded.model},
                         {"loaded_held", loaded.held_bytes},
                         {"memory_total", hw.memory_total},
                         {"disk_free", disk_free},
                         {"present_bytes", present}}},
        {"verdict", json{{"downloadable", v.downloadable}, {"mode", v.mode}, {"disk", v.disk}, {"memory", v.memory}}}};
    return true;
}

} // namespace bmoe::server
