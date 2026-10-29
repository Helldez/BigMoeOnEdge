package io.bigmoeonedge.app.data

import android.content.Context
import java.security.SecureRandom
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * The few settings that are the app's and not the engine's. Every engine tunable lives in the
 * server's layered settings and is rendered from its parameter table; what is here is how this
 * app runs the server (whether it listens on the network, with which key) and what it records.
 */
object AppPrefs {
    private const val FILE = "bmoe_app"

    /** Let the server listen on every interface, with the key, so other devices can use it. */
    fun shareOnNetwork(ctx: Context): Boolean = prefs(ctx).getBoolean("share", false)
    fun setShareOnNetwork(ctx: Context, on: Boolean) = prefs(ctx).edit().putBoolean("share", on).apply()

    /**
     * The key clients send as a bearer token when the server is shared. Generated once per install,
     * random, and shown in Settings; a new one is one tap away. Never sent anywhere by the app.
     */
    fun apiKey(ctx: Context): String {
        prefs(ctx).getString("api_key", null)?.let { return it }
        return regenerateApiKey(ctx)
    }

    fun regenerateApiKey(ctx: Context): String {
        val alphabet = "abcdefghjkmnpqrstuvwxyz23456789" // no look-alikes: a key is read off a screen
        val rnd = SecureRandom()
        val key = buildString { repeat(24) { append(alphabet[rnd.nextInt(alphabet.length)]) } }
        prefs(ctx).edit().putString("api_key", key).apply()
        return key
    }

    /**
     * Write one per-token CSV per loaded session and offer the Metrics screen (from Settings). Off
     * until asked for: a chat app records nothing by default, and the screen is a measuring tool.
     */
    fun metricsCsv(ctx: Context): Boolean = prefs(ctx).getBoolean("metrics_csv", false)
    fun setMetricsCsv(ctx: Context, on: Boolean) = prefs(ctx).edit().putBoolean("metrics_csv", on).apply()

    /** The appearances the app offers, as the stored value to its name: the same three words as the web UI. */
    val THEMES = listOf("system" to "System", "light" to "Light", "dark" to "Dark")

    /** A stored theme, or "system" for none or for a value this build does not know. */
    fun themeOf(stored: String?): String = stored?.takeIf { v -> THEMES.any { it.first == v } } ?: THEMES[0].first

    /** Whether [theme] draws dark, given what the system asks for. */
    fun darkFor(theme: String, systemDark: Boolean): Boolean = when (theme) {
        "light" -> false
        "dark" -> true
        else -> systemDark
    }

    // Observed, not just read: a choice made in Settings redraws the whole app at once.
    private val theme = MutableStateFlow("")

    /** The appearance chosen on this phone (system, light or dark), followed as it changes. */
    fun theme(ctx: Context): StateFlow<String> {
        if (theme.value.isEmpty()) theme.value = themeOf(prefs(ctx).getString("theme", null))
        return theme
    }
    fun setTheme(ctx: Context, value: String) {
        val v = themeOf(value)
        prefs(ctx).edit().putString("theme", v).apply()
        theme.value = v
    }

    /** The load whose context warning was dismissed on this phone, or "": one at a time is enough. */
    fun ctxWarningDismissed(ctx: Context): String = prefs(ctx).getString("ctx_warning_dismissed", null) ?: ""
    fun setCtxWarningDismissed(ctx: Context, key: String) = prefs(ctx).edit().putString("ctx_warning_dismissed", key).apply()

    /** Free the model after this long with no generation; the next message reloads it. */
    const val IDLE_UNLOAD_MINUTES = 10

    private fun prefs(ctx: Context) = ctx.getSharedPreferences(FILE, Context.MODE_PRIVATE)
}
