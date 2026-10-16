package io.bigmoeonedge.app.data

import android.content.Context
import java.security.SecureRandom

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

    /** Write one per-token CSV per loaded session, for the Metrics screen and for sharing. */
    fun metricsCsv(ctx: Context): Boolean = prefs(ctx).getBoolean("metrics_csv", true)
    fun setMetricsCsv(ctx: Context, on: Boolean) = prefs(ctx).edit().putBoolean("metrics_csv", on).apply()

    /** Free the model after this long with no generation; the next message reloads it. */
    const val IDLE_UNLOAD_MINUTES = 10

    private fun prefs(ctx: Context) = ctx.getSharedPreferences(FILE, Context.MODE_PRIVATE)
}
