package io.bigmoeonedge.app.engine

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.system.Os
import android.util.Log
import io.bigmoeonedge.app.data.AppPrefs
import io.bigmoeonedge.app.data.ModelManager
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.collect
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import java.io.BufferedReader
import java.io.File
import java.io.InputStreamReader
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread

/**
 * The engine, as a process of its own: `libbmoe-server.so`, the same bmoe-server the desktop
 * runs, started from the app's native library directory with its data beside the app's files.
 * The app is then an HTTP client of it on loopback, like the web UI is on a desktop
 * (docs/application-framework.md). A process of its own is the crash boundary: a driver that
 * ends the engine ends this process, not the app.
 */
class ServerProcess(private val ctx: Context) {
    @Volatile var process: Process? = null
        private set

    /** The last lines the server wrote to stderr: what to show when it dies. */
    private val errTail = StringBuilder()

    /**
     * Start the server, always with [apiKey] required: loopback on a phone is reachable by every
     * installed app, and without a key any of them could read a conversation off the event stream.
     * With [share] it listens on every interface too. The key travels in a file only this app can
     * read (`--api-key-file`), never on the command line. [onExit] runs once, on a thread of its
     * own, when the process ends.
     */
    fun start(share: Boolean, apiKey: String, onExit: (code: Int, stderrTail: String) -> Unit) {
        val nativeDir = ctx.applicationInfo.nativeLibraryDir
        val dataDir = dataDir(ctx)
        val argv = mutableListOf(
            ModelManager.serverPath(ctx),
            "--host", if (share) "0.0.0.0" else "127.0.0.1",
            "--port", PORT.toString(),
            "--data-dir", dataDir.absolutePath,
            "--models-dir", ModelManager.internalModelsDir(ctx).absolutePath,
            "--catalog", stageCatalog(ctx).absolutePath,
            "--api-key-file", writeKeyFile(ctx, apiKey).absolutePath,
        )
        // The other places a model can sit: the server lists what they hold and loads it in place.
        for (d in ModelManager.scanDirs(ctx)) argv += listOf("--scan-dir", d.absolutePath)

        val pb = ProcessBuilder(argv)
        pb.redirectErrorStream(false)
        // Where the system finds the engine's code, which only the app knows. How the backends are
        // configured once found is the server's (server/platform.cpp).
        pb.environment()["LD_LIBRARY_PATH"] = "$nativeDir:/system/lib64:/vendor/lib64"
        // The Hexagon backend loads its DSP-side skel (libggml-htp-v<arch>.so) through fastrpc, which
        // resolves it against ADSP_LIBRARY_PATH, not the linker path above. Without it the NPU
        // registers and then fails to open a session.
        pb.environment()["ADSP_LIBRARY_PATH"] = nativeDir
        pb.directory(dataDir)
        val p = pb.start()
        process = p
        errTail.setLength(0)
        thread(name = "bmoe-server-err") {
            try {
                BufferedReader(InputStreamReader(p.errorStream)).forEachLine { line ->
                    synchronized(errTail) {
                        errTail.append(line).append('\n')
                        if (errTail.length > 6000) errTail.delete(0, errTail.length - 4000)
                    }
                }
            } catch (_: Throwable) {
                // the stream closes with the process
            }
        }
        thread(name = "bmoe-server-out") {
            try {
                BufferedReader(InputStreamReader(p.inputStream)).forEachLine { /* the start-up banner */ }
            } catch (_: Throwable) {
            }
            val code = runCatching { p.waitFor() }.getOrDefault(-1)
            if (process === p) process = null
            onExit(code, synchronized(errTail) { errTail.toString() })
        }
    }

    /**
     * Ask the server to stop, and make sure it does: it holds a model-sized amount of memory. Blocks
     * for up to [EXIT_GRACE_MS] + [EXIT_FORCE_MS]: never call it on the main thread.
     */
    fun stop() {
        val p = process ?: return
        process = null
        runCatching { p.destroy() }
        runCatching {
            if (!p.waitFor(EXIT_GRACE_MS, TimeUnit.MILLISECONDS)) {
                p.destroyForcibly()
                p.waitFor(EXIT_FORCE_MS, TimeUnit.MILLISECONDS)
            }
        }
    }

    companion object {
        /** The one port the app's server listens on, and the one every address the app shows names. */
        const val PORT = 8765
        fun baseUrl() = "http://127.0.0.1:$PORT"

        /** The server's data directory (its settings.json and the catalog), beside the app's files. */
        fun dataDir(ctx: Context): File = File(ctx.filesDir, "bmoe").apply { mkdirs() }

        /**
         * The catalog is the project's one data file, shipped in the APK as an asset and copied out
         * on every start, so an app update brings the new catalog and the server reads a plain file.
         */
        fun stageCatalog(ctx: Context): File {
            val dir = File(dataDir(ctx), "catalog").apply { mkdirs() }
            val f = File(dir, "models.json")
            runCatching {
                ctx.assets.open("models.json").use { src -> f.outputStream().use { dst -> src.copyTo(dst) } }
            }
            return f
        }

        /**
         * The key, in a file of the app's private storage that only its owner may read or write
         * (0600), made so before the key is written, and kept out of backups. Rewritten every launch.
         */
        private fun writeKeyFile(ctx: Context, key: String): File {
            val f = File(ctx.noBackupFilesDir, "server-api-key")
            f.delete()
            f.createNewFile()
            Os.chmod(f.absolutePath, MODE_OWNER_RW)
            f.writeText(key)
            return f
        }

        private const val MODE_OWNER_RW = 0x180 // 0600
        private const val EXIT_GRACE_MS = 3000L
        private const val EXIT_FORCE_MS = 3000L
    }
}

/**
 * The foreground service that keeps the server process alive while the app is in the background
 * (a generation or a download in progress must not be killed with the activity) and holds a wake
 * lock while the engine generates. It owns the process; [Engine] owns the state and is what the
 * screens observe.
 *
 * Starting and stopping the process run one at a time, in order, on a background thread: a stop
 * waits for the process to end (seconds, for a model-sized one) and must not block the main thread,
 * and a restart must not start the next server before the previous one has let go of the port.
 */
class EngineService : Service() {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private var wake: PowerManager.WakeLock? = null
    private var watcher: Job? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                Engine.serverStopping()
                lifecycle { stopServer() }
                releaseWake()
                stopForegroundCompat()
                stopSelf()
                return START_NOT_STICKY
            }
            ACTION_RESTART -> {
                startForeground(NOTIF_ID, notification("Restarting the engine…"))
                Engine.serverStopping()
                lifecycle {
                    stopServer()
                    startServer()
                }
                if (watcher == null) watcher = scope.launch { follow() }
            }
            else -> {
                startForeground(NOTIF_ID, notification("Starting the engine…"))
                lifecycle { startServer() }
                if (watcher == null) watcher = scope.launch { follow() }
            }
        }
        return START_NOT_STICKY
    }

    private fun startServer() {
        if (server?.process != null) return
        val share = AppPrefs.shareOnNetwork(this)
        // Shared, the key is the one Settings shows, so a client keeps working across restarts; on
        // loopback alone nobody but this app needs it, and a fresh one per launch expires with it.
        val key = if (share) AppPrefs.apiKey(this) else Api.randomKey()
        val launch = Engine.serverStarting(share, key)
        val s = ServerProcess(applicationContext)
        server = s
        try {
            s.start(share, key) { code, tail -> Engine.serverExited(launch, code, tail) }
        } catch (t: Throwable) {
            server = null
            Engine.serverExited(launch, -1, t.message ?: t.toString())
        }
    }

    private fun stopServer() {
        server?.stop()
        server = null
    }

    /** The notification follows the engine, and the wake lock follows a generation. */
    private suspend fun follow() {
        Engine.state.map { Triple(it.serverUp, it.info?.state, it.info?.generating == true) }
            .distinctUntilChanged()
            .collect { (up, state, generating) ->
                val text = when {
                    !up -> "Engine not running"
                    generating -> "Generating…"
                    state == "planning" -> "Measuring the phone…"
                    state == "loading" -> "Loading model…"
                    state == "ready" -> "Model ready"
                    state == "error" -> "Engine error"
                    else -> "Engine ready"
                }
                runCatching { notify(text) }
                if (generating) acquireWake() else releaseWake()
            }
    }

    @Synchronized
    private fun acquireWake() {
        if (wake?.isHeld == true) return
        wake = (getSystemService(Context.POWER_SERVICE) as PowerManager)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "bmoe:gen")
            .apply { setReferenceCounted(false); acquire(60 * 60 * 1000L) }
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
        watcher?.cancel()
        // The process outlives this object for the seconds a stop takes: stopped on the lifecycle
        // thread, which this service's own scope (cancelled below) is not.
        lifecycle { stopServer() }
        releaseWake()
        scope.cancel()
        super.onDestroy()
    }

    private fun notification(text: String): Notification {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            nm.createNotificationChannel(NotificationChannel(CHANNEL, "Engine", NotificationManager.IMPORTANCE_LOW))
        }
        return Notification.Builder(this, CHANNEL)
            .setContentTitle("BigMoeOnEdge")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_sys_download)
            .setOngoing(true)
            .build()
    }

    private fun notify(text: String) {
        (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager).notify(NOTIF_ID, notification(text))
    }

    companion object {
        const val ACTION_START = "io.bigmoeonedge.app.ENGINE_START"
        const val ACTION_STOP = "io.bigmoeonedge.app.ENGINE_STOP"
        const val ACTION_RESTART = "io.bigmoeonedge.app.ENGINE_RESTART"
        private const val CHANNEL = "engine"
        private const val NOTIF_ID = 1

        // The process is the app's, not one service instance's: held here, and touched only by the
        // one consumer below, on the lifecycle scope, which no service's destruction cancels.
        private var server: ServerProcess? = null
        private val lifecycleScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

        // Starts and stops run one at a time and in the order they were asked for: a queue with a
        // single reader, where separate launches behind a lock would run in whatever order they
        // reached it, and a stop asked right after a start could end up before it.
        private val lifecycleQueue = Channel<() -> Unit>(Channel.UNLIMITED).also { q ->
            lifecycleScope.launch {
                for (block in q) runCatching(block).onFailure { Log.e("bmoe", "engine lifecycle step failed", it) }
            }
        }

        private fun lifecycle(block: () -> Unit) {
            lifecycleQueue.trySend(block)
        }
    }
}
