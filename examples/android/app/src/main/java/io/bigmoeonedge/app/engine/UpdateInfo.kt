package io.bigmoeonedge.app.engine

import org.json.JSONObject

/**
 * What `GET /api/update` answers (docs/server-api.md): this engine's version, the latest release's,
 * whether it is newer, and the page that says what changed. The feed carries no installer for a
 * phone (the APK ships beside it on the release), so the app sends the person to [notes] for it.
 */
data class UpdateInfo(val current: String, val latest: String, val available: Boolean, val notes: String) {
    /** The page to open: [notes] when it is https (the server checks the same), else none. */
    val releasePage: String? get() = notes.takeIf { it.startsWith("https://") }

    companion object {
        fun of(o: JSONObject) = UpdateInfo(
            current = o.optString("current"),
            latest = o.optString("latest"),
            available = o.optBoolean("available"),
            notes = o.optString("notes"),
        )
    }
}
