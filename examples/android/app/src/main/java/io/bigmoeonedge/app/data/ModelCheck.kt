package io.bigmoeonedge.app.data

import org.json.JSONObject

/**
 * The server's check of a catalog entry against this phone, before its download
 * (`POST /api/models/inspect`; docs/server-api.md, "Models"), as a first run holds it: asked when a
 * model is picked, kept for the visit, shown as the server worded it. No threshold and no wording
 * of the verdict lives here: the sentence and its tone are the server's. The web UI's
 * `ui/src/lib/modelCheck.ts`.
 */
data class ModelCheck(
    /** False when the storage cannot take the download, or the file is not a model. */
    val downloadable: Boolean,
    /** The answer in one sentence with no engine term; "" from an older server. */
    val summary: String,
    /** The tone it is said in: "ok", "warn", "err", or "" where nothing is claimed. */
    val tone: String,
    /** The engine's own line and its two sentences, for whoever asks how it was checked. */
    val headline: String,
    val disk: String,
    val memory: String,
    val mode: String,
) {
    /** The sentence to show: the plain one, or the headline of a server that has none. */
    val sentence: String get() = summary.ifEmpty { headline.ifEmpty { mode } }

    companion object {
        fun of(o: JSONObject): ModelCheck {
            val v = o.optJSONObject("verdict") ?: JSONObject()
            return ModelCheck(
                downloadable = v.optBoolean("downloadable", false),
                summary = v.optString("summary", ""),
                tone = v.optString("tone", ""),
                headline = v.optString("headline", ""),
                disk = v.optString("disk", ""),
                memory = v.optString("memory", ""),
                mode = v.optString("mode", ""),
            )
        }
    }
}

/** Where the check of one entry stands. */
sealed interface CheckState {
    data object Pending : CheckState
    data class Done(val check: ModelCheck) : CheckState
    data class Failed(val why: String) : CheckState
}

/** The tone the server said its sentence in; [Tone.NONE] where it claimed none, or while it is not known. */
fun checkTone(c: CheckState?): Tone = when ((c as? CheckState.Done)?.check?.tone) {
    "ok" -> Tone.OK
    "warn" -> Tone.WARN
    "err" -> Tone.ERR
    else -> Tone.NONE
}

/**
 * Whether the download may start: not while the check runs (its answer is a second away), and not
 * when the server said the model cannot be downloaded here. A check that could not be made refuses
 * nothing: the download has its own checks.
 */
fun mayDownload(c: CheckState?): Boolean = when (c) {
    null, CheckState.Pending -> false
    is CheckState.Failed -> true
    is CheckState.Done -> c.check.downloadable
}
