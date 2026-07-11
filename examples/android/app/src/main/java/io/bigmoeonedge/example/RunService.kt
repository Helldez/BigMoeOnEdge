package io.bigmoeonedge.example

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import java.io.BufferedReader
import java.io.File
import java.io.InputStreamReader
import kotlin.concurrent.thread

/**
 * Runs bmoe-cli as a foreground service so generation survives screen-off. The CLI is a
 * native executable (shipped as libbmoe-cli.so); we exec it from nativeLibraryDir with
 * LD_LIBRARY_PATH pointed at the bundled libllama/libggml .so files, and stream its
 * stdout into the RunBus for the UI to render.
 */
class RunService : Service() {

    private val telemetry = TelemetryParser()
    private var proc: Process? = null
    private var wake: PowerManager.WakeLock? = null

    // Set when the user presses Stop: the CLI is then killed on purpose, so its non-zero
    // exit code (SIGTERM) must not be surfaced as an error.
    @Volatile
    private var stopping = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_STOP) { stopEverything(); return START_NOT_STICKY }

        startForeground(NOTIF_ID, buildNotification("Generating…"))

        val model = intent?.getStringExtra(EXTRA_MODEL) ?: run { finishWithError("no model"); return START_NOT_STICKY }
        val prompt = intent.getStringExtra(EXTRA_PROMPT) ?: ""
        val argv = intent.getStringArrayListExtra(EXTRA_ARGV) ?: run { finishWithError("no argv"); return START_NOT_STICKY }

        RunBus.reset()
        RunBus.setRunning(true)
        RunBus.setLoading(true) // held until the first token arrives (model load + prompt eval)

        wake = (getSystemService(Context.POWER_SERVICE) as PowerManager)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "bmoe:gen").apply { acquire(30 * 60 * 1000L) }

        thread(name = "bmoe-cli") { runCli(argv, model, prompt) }
        return START_NOT_STICKY
    }

    private fun runCli(argv: ArrayList<String>, model: String, prompt: String) {
        try {
            val nativeDir = applicationInfo.nativeLibraryDir
            val pb = ProcessBuilder(argv)
            pb.redirectErrorStream(false)
            pb.environment()["LD_LIBRARY_PATH"] = "$nativeDir:/system/lib64:/vendor/lib64"
            pb.directory(File(model).parentFile)

            val p = pb.start().also { proc = it }

            // Drain stderr so the CLI never blocks on a full pipe; surface the tail on error.
            val errTail = StringBuilder()
            thread(name = "bmoe-cli-err") {
                BufferedReader(InputStreamReader(p.errorStream)).forEachLine {
                    if (errTail.length < 4000) errTail.append(it).append('\n')
                }
            }

            BufferedReader(InputStreamReader(p.inputStream)).useLines { lines ->
                lines.forEach { line ->
                    if (telemetry.onLine(line)) {
                        RunBus.update {
                            it.copy(
                                loading = false, // first telemetry line means the model is up
                                telemetry = telemetry.current.copy(),
                                summary = telemetry.summary,
                                answer = telemetry.current.text,
                            )
                        }
                    }
                }
            }

            val code = p.waitFor()
            if (code != 0 && !stopping) {
                RunBus.update {
                    if (it.error == null) it.copy(error = "bmoe-cli exited $code\n${errTail.takeLast(1200)}") else it
                }
            }
        } catch (t: Throwable) {
            if (!stopping) RunBus.update { it.copy(error = t.message ?: t.toString()) }
        } finally {
            RunBus.setRunning(false)
            releaseWake()
            stopForegroundCompat()
            stopSelf()
        }
    }

    private fun finishWithError(msg: String) {
        RunBus.update { it.copy(error = msg, running = false) }
        stopForegroundCompat()
        stopSelf()
    }

    private fun stopEverything() {
        stopping = true
        proc?.destroy()
        RunBus.setRunning(false)
        releaseWake()
        stopForegroundCompat()
        stopSelf()
    }

    private fun releaseWake() {
        wake?.let { if (it.isHeld) it.release() }
        wake = null
    }

    private fun stopForegroundCompat() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) stopForeground(STOP_FOREGROUND_REMOVE)
        else @Suppress("DEPRECATION") stopForeground(true)
    }

    override fun onDestroy() {
        proc?.destroy()
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

    companion object {
        const val ACTION_STOP = "io.bigmoeonedge.example.STOP"
        const val EXTRA_MODEL = "model"
        const val EXTRA_PROMPT = "prompt"
        const val EXTRA_ARGV = "argv"
        private const val CHANNEL = "gen"
        private const val NOTIF_ID = 1
    }
}
