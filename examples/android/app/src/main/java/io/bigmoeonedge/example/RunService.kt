package io.bigmoeonedge.example

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import org.json.JSONObject
import java.io.BufferedReader
import java.io.BufferedWriter
import java.io.File
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import kotlin.concurrent.thread

/**
 * Hosts ONE persistent bmoe-cli session as a foreground service. The native CLI (shipped as
 * libbmoe-cli.so) is exec'd once with `--session`; the model load and the expert-cache warm-up
 * are paid a single time, and every subsequent prompt is sent as a JSON line on the process's
 * stdin, keeping the cache warm between prompts. Its BMOE_* stdout drives [RunBus].
 *
 * Lifecycle: START_SESSION spawns the process (LOADING → READY); GENERATE sends one prompt
 * (GENERATING → READY); CANCEL interrupts the current generation without unloading; SHUTDOWN (or
 * an idle timeout) closes the process and frees the model.
 */
class RunService : Service() {

    private val telemetry = TelemetryParser()
    @Volatile private var proc: Process? = null
    @Volatile private var procWriter: BufferedWriter? = null
    @Volatile private var wake: PowerManager.WakeLock? = null

    @Volatile private var sessionSig: String? = null
    @Volatile private var shuttingDown = false
    private var nextId = 1

    // A prompt supplied with START_SESSION runs as soon as the process reports READY.
    @Volatile private var pending: Req? = null

    private val writeLock = Any()
    private val main = Handler(Looper.getMainLooper())
    private val idleUnload = Runnable { shutdownSession() }

    private data class Req(val prompt: String, val nPredict: Int, val think: Boolean)

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_GENERATE -> sendGenerate(reqFrom(intent))
            ACTION_CANCEL -> send("""{"cmd":"cancel"}""")
            ACTION_SHUTDOWN -> shutdownSession()
            else -> startSession(intent)
        }
        return START_NOT_STICKY
    }

    // ── session lifecycle ──

    private fun startSession(intent: Intent?) {
        val model = intent?.getStringExtra(EXTRA_MODEL) ?: run { fail("no model"); return }
        val argv = intent.getStringArrayListExtra(EXTRA_ARGV) ?: run { fail("no argv"); return }
        val sig = intent.getStringExtra(EXTRA_SIG)
        val req = if (intent.hasExtra(EXTRA_PROMPT)) reqFrom(intent) else null

        // Already running the requested session? Just generate against the warm process.
        if (proc != null && sig == sessionSig && !shuttingDown) {
            if (req != null) sendGenerate(req)
            return
        }
        // Different model/settings (or nothing running): tear down and start fresh.
        killProcess()
        shuttingDown = false
        pending = req
        sessionSig = sig
        main.removeCallbacks(idleUnload)

        startForeground(NOTIF_ID, buildNotification("Loading model…"))
        RunBus.update { it.copy(state = EngineState.LOADING, error = null, sessionSig = sig, answer = "", summary = "") }

        thread(name = "bmoe-session") { runSession(argv, model) }
    }

    private fun runSession(argv: ArrayList<String>, model: String) {
        try {
            val nativeDir = applicationInfo.nativeLibraryDir
            val pb = ProcessBuilder(argv)
            pb.redirectErrorStream(false)
            pb.environment()["LD_LIBRARY_PATH"] = "$nativeDir:/system/lib64:/vendor/lib64"
            pb.directory(File(model).parentFile)

            val p = pb.start().also { proc = it }
            procWriter = BufferedWriter(OutputStreamWriter(p.outputStream))

            // Drain stderr; surface the tail on unexpected exit and sniff the effective read mode.
            // Guarded: killProcess() closes this stream and would otherwise crash the whole app.
            val errTail = StringBuilder()
            thread(name = "bmoe-session-err") {
                try {
                    BufferedReader(InputStreamReader(p.errorStream)).forEachLine { line ->
                        if (errTail.length < 4000) errTail.append(line).append('\n')
                        when {
                            "O_DIRECT returns wrong data" in line ->
                                RunBus.update { it.copy(ioMode = "buffered (O_DIRECT unsupported on this storage)") }
                            "expert streaming ON" in line ->
                                Regex("""o_direct=(\d)""").find(line)?.groupValues?.get(1)?.let { d ->
                                    RunBus.update {
                                        if (it.ioMode != null) it
                                        else it.copy(ioMode = if (d == "1") "direct (O_DIRECT)" else "buffered")
                                    }
                                }
                        }
                    }
                } catch (_: Throwable) {
                    // stream closed on shutdown — nothing to surface.
                }
            }

            BufferedReader(InputStreamReader(p.inputStream)).useLines { lines ->
                lines.forEach { handleLine(it) }
            }

            val code = p.waitFor()
            if (!shuttingDown && code != 0) {
                RunBus.update {
                    it.copy(state = EngineState.ERROR,
                        error = if (it.error == null) "bmoe-cli exited $code\n${errTail.takeLast(1200)}" else it.error)
                }
            }
        } catch (t: Throwable) {
            if (!shuttingDown) RunBus.update { it.copy(state = EngineState.ERROR, error = t.message ?: t.toString()) }
        } finally {
            releaseWake()
            procWriter = null
            proc = null
            if (!shuttingDown) RunBus.update { if (it.state != EngineState.ERROR) it.copy(state = EngineState.IDLE, sessionSig = null) else it.copy(sessionSig = null) }
            main.post {
                stopForegroundCompat()
                stopSelf()
            }
        }
    }

    // ── stdout line protocol (docs/telemetry.md) ──

    private fun handleLine(line: String) {
        val t = line.trim()
        when {
            t.startsWith("BMOE_READY ") -> {
                RunBus.setState(EngineState.READY)
                main.post { notify("Model ready") }
                pending?.let { p -> pending = null; sendGenerate(p) } ?: scheduleIdleUnload()
            }
            t.startsWith("BMOE_BEGIN ") -> {
                telemetry.reset()
                acquireWake()
                main.removeCallbacks(idleUnload)
                RunBus.update {
                    it.copy(state = EngineState.GENERATING, telemetry = telemetry.current.copy(),
                        answer = "", summary = "", error = null)
                }
                main.post { notify("Generating…") }
            }
            telemetry.onLine(t) -> RunBus.update {
                it.copy(telemetry = telemetry.current.copy(), answer = telemetry.current.text)
            }
            t.startsWith("BMOE_DONE ") -> onDone(t.removePrefix("BMOE_DONE "))
            t.startsWith("BMOE_ERROR ") -> onError(t.removePrefix("BMOE_ERROR "))
        }
    }

    private fun onDone(json: String) {
        runCatching {
            val o = JSONObject(json)
            val tokens = o.optInt("tokens")
            val tokS = o.optDouble("tok_s")
            val hit = o.optDouble("cache_hit_pct", -1.0)
            val prefill = o.optDouble("prefill_s")
            val cancelled = o.optBoolean("cancelled")
            val text = o.optString("text")
            val summary = buildString {
                append(String.format(java.util.Locale.US, "generation: %d tokens (%.2f tok/s)", tokens, tokS))
                if (prefill > 0) append(String.format(java.util.Locale.US, " | prefill %.2fs", prefill))
                if (hit >= 0) append(String.format(java.util.Locale.US, " | cache %.0f%%", hit))
                if (cancelled) append(" | cancelled")
            }
            val tel = telemetry.current.copy(avgTokensPerSecond = tokS)
            RunBus.update {
                it.copy(state = EngineState.READY, telemetry = tel,
                    answer = if (text.isNotEmpty()) text else it.answer, summary = summary)
            }
        }
        releaseWake()
        main.post { notify("Model ready") }
        scheduleIdleUnload()
    }

    private fun onError(json: String) {
        val fatal = runCatching { JSONObject(json).optBoolean("fatal", true) }.getOrDefault(true)
        val msg = runCatching { JSONObject(json).optString("msg") }.getOrDefault("engine error")
        releaseWake()
        if (fatal) {
            RunBus.update { it.copy(state = EngineState.ERROR, error = msg) }
            shutdownSession()
        } else {
            // Bad request (e.g. context overflow): the session stays loaded and usable.
            RunBus.update { it.copy(state = EngineState.READY, error = msg) }
            main.post { notify("Model ready") }
            scheduleIdleUnload()
        }
    }

    // ── requests ──

    private fun reqFrom(intent: Intent): Req = Req(
        prompt = intent.getStringExtra(EXTRA_PROMPT) ?: "",
        nPredict = intent.getIntExtra(EXTRA_NPREDICT, 48),
        think = intent.getBooleanExtra(EXTRA_THINK, false),
    )

    private fun sendGenerate(req: Req) {
        val id = nextId++
        val json = buildString {
            append("""{"cmd":"generate","id":""").append(id)
            append(""","n_predict":""").append(req.nPredict)
            append(""","think":""").append(req.think)
            append(""","clear_kv":true""")
            append(""","prompt":"""").append(jsonEscape(req.prompt)).append("\"}")
        }
        if (!send(json)) fail("session not ready")
    }

    private fun send(json: String): Boolean = synchronized(writeLock) {
        val w = procWriter ?: return false
        return try {
            w.write(json); w.write("\n"); w.flush(); true
        } catch (_: Throwable) {
            false
        }
    }

    // ── teardown ──

    private fun shutdownSession() {
        shuttingDown = true
        main.removeCallbacks(idleUnload)
        send("""{"cmd":"close"}""")
        // Give the process a moment to exit cleanly on close, then force it.
        main.postDelayed({ killProcess() }, 1500)
        RunBus.update { it.copy(state = EngineState.IDLE, sessionSig = null) }
    }

    private fun killProcess() {
        synchronized(writeLock) {
            runCatching { procWriter?.close() }
            procWriter = null
        }
        runCatching { proc?.destroy() }
        proc = null
    }

    private fun scheduleIdleUnload() {
        main.removeCallbacks(idleUnload)
        main.postDelayed(idleUnload, IDLE_UNLOAD_MS)
    }

    private fun fail(msg: String) {
        RunBus.update { it.copy(state = EngineState.ERROR, error = msg) }
        stopForegroundCompat()
        stopSelf()
    }

    private fun acquireWake() {
        if (wake?.isHeld == true) return
        wake = (getSystemService(Context.POWER_SERVICE) as PowerManager)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "bmoe:gen")
            .apply { setReferenceCounted(false); acquire(30 * 60 * 1000L) }
    }

    @Synchronized
    private fun releaseWake() {
        wake?.let { if (it.isHeld) it.release() }
        wake = null
    }

    private fun stopForegroundCompat() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) stopForeground(STOP_FOREGROUND_REMOVE)
        else @Suppress("DEPRECATION") stopForeground(true)
    }

    override fun onDestroy() {
        shuttingDown = true
        main.removeCallbacks(idleUnload)
        killProcess()
        releaseWake()
        super.onDestroy()
    }

    private fun buildNotification(text: String): Notification {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            nm.createNotificationChannel(
                NotificationChannel(CHANNEL, "Generation", NotificationManager.IMPORTANCE_LOW)
            )
        }
        return Notification.Builder(this, CHANNEL)
            .setContentTitle("BigMoeOnEdge")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_sys_download)
            .setOngoing(true)
            .build()
    }

    private fun notify(text: String) {
        (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager)
            .notify(NOTIF_ID, buildNotification(text))
    }

    companion object {
        const val ACTION_GENERATE = "io.bigmoeonedge.example.GENERATE"
        const val ACTION_CANCEL = "io.bigmoeonedge.example.CANCEL"
        const val ACTION_SHUTDOWN = "io.bigmoeonedge.example.SHUTDOWN"
        const val EXTRA_MODEL = "model"
        const val EXTRA_ARGV = "argv"
        const val EXTRA_SIG = "sig"
        const val EXTRA_PROMPT = "prompt"
        const val EXTRA_NPREDICT = "n_predict"
        const val EXTRA_THINK = "think"
        private const val CHANNEL = "gen"
        private const val NOTIF_ID = 1

        // Free the model after this long with no generation, so an idle session does not hold
        // ~model-sized RAM and a foreground service indefinitely. The next prompt reloads.
        private const val IDLE_UNLOAD_MS = 10 * 60 * 1000L

        private fun jsonEscape(s: String): String {
            val o = StringBuilder(s.length + 8)
            for (c in s) when (c) {
                '"' -> o.append("\\\"")
                '\\' -> o.append("\\\\")
                '\n' -> o.append("\\n")
                '\r' -> o.append("\\r")
                '\t' -> o.append("\\t")
                else -> if (c < ' ') o.append(String.format("\\u%04x", c.code)) else o.append(c)
            }
            return o.toString()
        }
    }
}
