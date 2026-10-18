package io.bigmoeonedge.app.data

import org.json.JSONObject

/**
 * A model from a Hugging Face repository, as the server reports it (docs/server-api.md, "Models"):
 * what a repository holds (`POST /api/hf/models`), what one model is before a byte of it is
 * downloaded (`POST /api/hf/inspect`), and the catalog entry it became (`POST /api/hf/add`). The
 * app shows these and downloads the entry's files itself, as it does every catalog entry; nothing
 * here reads a gguf or names an engine parameter.
 */
data class HfModel(
    /** The first file's path in the repository: what the model is called in every request. */
    val name: String,
    val quant: String,
    val bytes: Long,
    /** Shards the set declares, and whether the listing has them all. */
    val count: Int,
    val complete: Boolean,
) {
    companion object {
        fun of(o: JSONObject): HfModel = HfModel(
            name = o.getString("name"),
            quant = o.optString("quant", ""),
            bytes = o.optLong("bytes", 0),
            count = o.optInt("count", 1),
            complete = o.optBoolean("complete", true),
        )
    }
}

data class HfModels(
    val repo: String,
    val revision: String,
    /** The file the request's URL named, "" when it named none. */
    val file: String,
    val models: List<HfModel>,
) {
    companion object {
        fun of(o: JSONObject): HfModels = HfModels(
            repo = o.getString("repo"),
            revision = o.optString("revision", "main"),
            file = o.optString("file", ""),
            models = o.optJSONArray("models")?.let { a -> (0 until a.length()).map { i -> HfModel.of(a.getJSONObject(i)) } }.orEmpty(),
        )
    }
}

/** The facts the server read off a model's header, and its verdict on this phone. */
data class HfFacts(
    val name: String,
    val quant: String,
    val bytes: Long,
    val files: Int,
    val complete: Boolean,
    val readable: Boolean,
    val arch: String,
    val isMoe: Boolean,
    val nExpert: Int,
    val nExpertUsed: Int,
    val nLayer: Int,
    val nMoeLayer: Int,
    val denseBytes: Long,
    val expertBytes: Long,
    val tokenCycleBytes: Long,
    val streamable: Boolean,
    val refusal: String,
    /** False only when the disk cannot take the download. */
    val downloadable: Boolean,
    /** "streams", "plain mmap" or "not readable". */
    val mode: String,
    val disk: String,
    val memory: String,
) {
    companion object {
        fun of(o: JSONObject): HfFacts {
            val m = o.optJSONObject("model") ?: JSONObject()
            val v = o.optJSONObject("verdict") ?: JSONObject()
            return HfFacts(
                name = o.getString("name"),
                quant = o.optString("quant", ""),
                bytes = o.optLong("bytes", 0),
                files = o.optInt("files", 1),
                complete = o.optBoolean("complete", true),
                readable = m.optBoolean("readable", false),
                arch = m.optString("arch", ""),
                isMoe = m.optBoolean("is_moe", false),
                nExpert = m.optInt("n_expert", 0),
                nExpertUsed = m.optInt("n_expert_used", 0),
                nLayer = m.optInt("n_layer", 0),
                nMoeLayer = m.optInt("n_moe_layer", 0),
                denseBytes = m.optLong("dense_bytes", 0),
                expertBytes = m.optLong("expert_bytes", 0),
                tokenCycleBytes = m.optLong("token_cycle_bytes", 0),
                streamable = m.optBoolean("streamable", false),
                refusal = m.optString("refusal", ""),
                downloadable = v.optBoolean("downloadable", false),
                mode = v.optString("mode", ""),
                disk = v.optString("disk", ""),
                memory = v.optString("memory", ""),
            )
        }
    }
}

/** The body of the three requests: the repository as typed, the model's name where one is meant, a token where given. */
fun hfRequestBody(repo: String, name: String? = null, token: String? = null): JSONObject = JSONObject().apply {
    put("repo", repo.trim())
    if (!name.isNullOrEmpty()) put("name", name)
    if (!token.isNullOrBlank()) put("token", token.trim())
}
