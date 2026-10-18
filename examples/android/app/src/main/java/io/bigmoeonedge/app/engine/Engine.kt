package io.bigmoeonedge.app.engine

import android.content.Context
import android.content.Intent
import androidx.core.content.ContextCompat
import io.bigmoeonedge.app.data.AppPrefs
import io.bigmoeonedge.app.data.CatalogEntry
import io.bigmoeonedge.app.data.MetricsRecorder
import io.bigmoeonedge.app.data.Telemetry
import io.bigmoeonedge.app.data.Thermal
import io.bigmoeonedge.app.data.applyDone
import io.bigmoeonedge.app.data.applyToken
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.net.ConnectException
import java.net.SocketTimeoutException
import java.util.concurrent.atomic.AtomicLong
import kotlin.coroutines.cancellation.CancellationException

// ── what the server says, as the screens read it ──

data class ModelInfo(
    val name: String,
    val path: String,
    val arch: String,
    val nCtx: Int,
    val nExpertUsed: Int,
    val thinkCtl: String, // template | prefill | none
    val loadS: Double,
    val streaming: Boolean,
    /** What a CPU figure is read against: the session's compute threads, and every busy thread. */
    val computeThreads: Int,
    val busyThreads: Int,
) {
    companion object {
        fun of(o: JSONObject?): ModelInfo? {
            if (o == null) return null
            return ModelInfo(
                o.optString("name"), o.optString("path"), o.optString("arch"), o.optInt("n_ctx"),
                o.optInt("n_expert_used"), o.optString("think_ctl", "template"), o.optDouble("load_s", 0.0),
                o.optBoolean("streaming", false), o.optInt("compute_threads", 0), o.optInt("busy_threads", 0),
            )
        }
    }
}

/** `GET /api/info`, and every `state` event. */
data class ServerInfo(
    val version: String,
    val plannerAvailable: Boolean,
    val state: String, // empty | loading | ready | error
    val error: String,
    val model: ModelInfo?,
    val generating: Boolean,
    val ramTotalMib: Long,
    val ramAvailableMib: Long,
) {
    val ready get() = state == "ready" && model != null

    companion object {
        fun of(o: JSONObject): ServerInfo {
            val host = o.optJSONObject("host")
            return ServerInfo(
                version = o.optString("version"),
                plannerAvailable = o.optBoolean("planner_available", false),
                state = o.optString("state", "empty"),
                error = o.optString("error"),
                model = ModelInfo.of(o.optJSONObject("model")),
                generating = o.optBoolean("generating", false),
                ramTotalMib = host?.optLong("ram_total_mib", 0) ?: 0,
                ramAvailableMib = host?.optLong("ram_available_mib", 0) ?: 0,
            )
        }
    }
}

data class PlanDecision(val knob: String, val value: String, val source: String, val reason: String) {
    companion object {
        fun list(arr: JSONArray?): List<PlanDecision> = arr?.let { a ->
            (0 until a.length()).map { i ->
                val d = a.getJSONObject(i)
                PlanDecision(d.optString("knob"), d.optString("value"), d.optString("source"), d.optString("reason"))
            }
        }.orEmpty()
    }
}

/** `GET /api/config`, and every `config` event: the values, and which of them the person set. */
data class ConfigState(
    val values: Map<String, Any?>,
    val userKeys: Set<String>,
    val valid: Boolean,
    val error: String,
    val reloadRequired: Boolean,
    val plan: List<PlanDecision>?,
    val autoPlan: Boolean,
) {
    fun value(key: String): Any? = values[key]
    fun text(key: String): String = values[key]?.toString().orEmpty()

    companion object {
        fun of(o: JSONObject): ConfigState {
            val v = o.optJSONObject("values") ?: JSONObject()
            val values = v.keys().asSequence().associateWith { k -> if (v.isNull(k)) null else v.get(k) }
            val uk = o.optJSONArray("user_keys")?.let { a -> (0 until a.length()).map { a.getString(it) } }.orEmpty()
            return ConfigState(
                values = values,
                userKeys = uk.toSet(),
                valid = o.optBoolean("valid", true),
                error = o.optString("error"),
                reloadRequired = o.optBoolean("reload_required", false),
                plan = if (o.isNull("plan")) null else PlanDecision.list(o.optJSONArray("plan")),
                autoPlan = o.optBoolean("auto_plan", false),
            )
        }
    }
}

data class LocalModel(
    val name: String,
    val path: String,
    /** The directory the server found it in: its models directory, or one the app gave it to scan. */
    val dir: String,
    val bytes: Long,
    val shards: Int,
    /** Every file of the model that is present, in shard order: what a delete removes. */
    val files: List<String>,
    val arch: String,
    val streamable: Boolean,
    val complete: Boolean,
)

/** `GET /api/models`: the server's directories, and the catalog with the server's view of each entry. */
data class ModelsList(
    val modelsDir: String,
    val scanDirs: List<String>,
    val local: List<LocalModel>,
    val catalog: List<CatalogEntry>,
) {
    /** The name of every model file the server sees, shards included: what "on the device" means. */
    val fileNames: Set<String> get() = local.flatMap { m -> m.files.map { it.substringAfterLast('/') } }.toSet()

    companion object {
        private fun strings(a: JSONArray?): List<String> = a?.let { (0 until it.length()).map { i -> it.getString(i) } }.orEmpty()

        fun of(o: JSONObject): ModelsList {
            val local = o.optJSONArray("local")?.let { a ->
                (0 until a.length()).map { i ->
                    val m = a.getJSONObject(i)
                    LocalModel(
                        name = m.optString("name"),
                        path = m.optString("path"),
                        dir = m.optString("dir"),
                        bytes = m.optLong("bytes"),
                        shards = m.optInt("shards", 1),
                        files = strings(m.optJSONArray("files")),
                        arch = m.optString("arch"),
                        streamable = m.optBoolean("streamable", false),
                        complete = m.optBoolean("complete", true),
                    )
                }
            }.orEmpty()
            val catalog = o.optJSONArray("catalog")?.let { a ->
                (0 until a.length()).mapNotNull { i -> CatalogEntry.of(a.getJSONObject(i)) }
            }.orEmpty()
            return ModelsList(o.optString("models_dir"), strings(o.optJSONArray("scan_dirs")), local, catalog)
        }
    }
}

/** `GET /api/plan`. */
data class PlanResult(
    val available: Boolean,
    val error: String,
    val model: String,
    val machine: String,
    val regime: String,
    val declined: Boolean,
    val declineReason: String,
    val decisions: List<PlanDecision>,
    val args: List<String>,
    val notApplicable: List<String>,
    val warning: String,
    val fromLastLoad: Boolean,
    val explain: String,
) {
    companion object {
        fun of(o: JSONObject): PlanResult = PlanResult(
            available = o.optBoolean("available", false),
            error = o.optString("error").ifEmpty { o.optString("reason") },
            model = o.optString("model"),
            machine = o.optString("machine"),
            regime = o.optString("regime"),
            declined = o.optBoolean("streaming_declined", false),
            declineReason = o.optString("decline_reason"),
            decisions = PlanDecision.list(o.optJSONArray("decisions")),
            args = o.optJSONArray("args")?.let { a -> (0 until a.length()).map { a.getString(it) } }.orEmpty(),
            notApplicable = o.optJSONArray("not_applicable")?.let { a -> (0 until a.length()).map { a.getString(it) } }.orEmpty(),
            warning = o.optString("warning"),
            fromLastLoad = o.optBoolean("from_last_load", false),
            explain = o.optString("explain"),
        )
    }
}

/** Everything the screens observe about the engine. Immutable snapshots on one StateFlow. */
data class EngineUi(
    val serverUp: Boolean = false,
    /** Why the server is not up: it has not started yet, it died (with its stderr), it was stopped. */
    val serverError: String? = null,
    /** How the running server was started: on the network with the key, or on loopback. */
    val shared: Boolean = false,
    val info: ServerInfo? = null,
    val params: List<ParamSpec>? = null,
    val config: ConfigState? = null,
    val models: ModelsList? = null,
    val live: Telemetry = Telemetry(),
    val lastDone: JSONObject? = null,
    val cpuTempC: Double? = null,
) {
    val ready get() = serverUp && info?.ready == true
    val generating get() = info?.generating == true
    val loading get() = info?.state == "loading" || info?.state == "planning"
}

/**
 * The one engine, as this app sees it: a StateFlow fed by the server's event stream, and the
 * calls the screens make. The server is a process of its own, a child that [EngineService] starts
 * and keeps; this object lives in the app's process, connects to loopback with the key that launch
 * was given, checks the server it reaches is that one, and keeps reconnecting, so a screen never
 * has to know whether the server is up, only what the state says.
 *
 * Nothing here knows an engine parameter by name beyond the two the chat itself needs: the
 * settings screen renders whatever `GET /api/params` describes.
 */
object Engine {
    private val _state = MutableStateFlow(EngineUi())
    val state: StateFlow<EngineUi> = _state.asStateFlow()

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    // Until a launch says otherwise, a key no server has: nothing it reaches is taken for the app's.
    @Volatile private var api = Api(ServerProcess.baseUrl(), Api.randomKey())
    @Volatile private var connector: Job? = null
    @Volatile private var idleUnload: Job? = null
    @Volatile private var thermal: Thermal? = null
    @Volatile private var recorder: MetricsRecorder? = null
    @Volatile private var appContext: Context? = null
    @Volatile private var lastThermalSampleMs = 0L

    // Each start of the server is a launch with its own number; what a process reports about itself
    // (its exit) counts only while it is still the current launch. A stop moves the number on, so
    // the exit of the server it stopped, which arrives seconds later, changes nothing.
    private val launches = AtomicLong(0)
    @Volatile private var currentLaunch = 0L
    // Whether a server this app started should be up: between a stop and the next start the
    // connector waits, rather than reconnect to the process that is still on its way out.
    @Volatile private var serving = false

    val current: EngineUi get() = _state.value

    /** Start the service (and so the server) if it is not running, and keep the state following it. */
    fun start(ctx: Context) {
        val app = attach(ctx)
        ContextCompat.startForegroundService(app, Intent(app, EngineService::class.java).setAction(EngineService.ACTION_START))
    }

    /** What follows the server from this process: the event stream, the thermal sensor, the recorder. */
    @Synchronized
    private fun attach(ctx: Context): Context {
        val app = ctx.applicationContext
        appContext = app
        if (thermal == null) thermal = Thermal(app)
        if (recorder == null) recorder = MetricsRecorder(app)
        if (connector == null) connector = scope.launch { connectForever() }
        return app
    }

    /** Stop the server (and free the model). The next [start] brings it back. */
    fun stop(ctx: Context) {
        val app = ctx.applicationContext
        app.startService(Intent(app, EngineService::class.java).setAction(EngineService.ACTION_STOP))
    }

    /**
     * Apply a changed sharing setting or key: the server is started again with the other address.
     * The service stops the running one and starts the next only once it has ended, so the two
     * never race for the port.
     */
    fun restart(ctx: Context) {
        val app = attach(ctx)
        ContextCompat.startForegroundService(app, Intent(app, EngineService::class.java).setAction(EngineService.ACTION_RESTART))
    }

    // ── what the service reports ──

    /** A server is about to start with [apiKey]: the client speaks to it, and its launch number is returned. */
    @Synchronized
    internal fun serverStarting(share: Boolean, apiKey: String): Long {
        val launch = launches.incrementAndGet()
        currentLaunch = launch
        api = Api(ServerProcess.baseUrl(), apiKey)
        serving = true
        _state.update { it.copy(serverError = null, shared = share) }
        return launch
    }

    @Synchronized
    internal fun serverStopping() {
        serving = false
        currentLaunch = launches.incrementAndGet()
        recorder?.close()
        _state.update { it.copy(serverUp = false, serverError = "The engine was stopped.", info = null) }
    }

    @Synchronized
    internal fun serverExited(launch: Long, code: Int, stderrTail: String) {
        if (launch != currentLaunch) return // a server already stopped or replaced
        recorder?.close()
        val why = if (code == 0) "The engine stopped." else "The engine process ended (exit $code)."
        val tail = stderrTail.trim().takeLast(1500)
        _state.update {
            it.copy(serverUp = false, serverError = if (tail.isEmpty()) why else "$why\n$tail", info = null)
        }
    }

    // ── the event stream ──

    private suspend fun connectForever() {
        var backoffMs = 500L
        while (true) {
            if (!serving) {
                if (_state.value.serverUp) _state.update { it.copy(serverUp = false) }
                delay(500L)
                continue
            }
            val a = api
            try {
                if (!a.isOurs()) {
                    // Something answers on the port, and it is not the server this launch started:
                    // another app's, or one left over. A conversation is not handed to it.
                    if (a === api) {
                        _state.update {
                            it.copy(
                                serverUp = false,
                                info = null,
                                serverError = "Port ${ServerProcess.PORT} is held by a server this app did not start, so " +
                                    "the engine cannot listen there. Close the program that holds it (or force-stop " +
                                    "this app if it is an old engine of its own), then start the engine again.",
                            )
                        }
                    }
                    delay(5_000L)
                    continue
                }
                // Only while this launch is the one being served: after a stop or a restart the old
                // process can still send a ping or a state frame for the seconds it takes to end, and
                // one of those must not mark the engine up again or reach the screens.
                val current = { serving && a === api }
                a.events(
                    onEvent = { ev ->
                        if (current()) {
                            if (!_state.value.serverUp) {
                                _state.update { it.copy(serverUp = true, serverError = null) }
                                backoffMs = 500L
                                scope.launch { refreshStatic() }
                            }
                            onEvent(ev)
                        }
                    },
                    keepGoing = current,
                )
            } catch (c: CancellationException) {
                throw c
            } catch (_: ConnectException) {
                // A closed port: not up yet, or gone. Reconnecting below is the whole answer.
            } catch (_: SocketTimeoutException) {
                // The watchdog: two pings missed, the stream died without closing. Reconnect now.
                backoffMs = 500L
            } catch (t: Throwable) {
                // Anything else (a key, a policy, a broken stream) is what a person needs to read.
                val why = t.message?.takeIf { it.isNotBlank() } ?: t.javaClass.simpleName
                _state.update { it.copy(serverError = "Engine not reachable: $why") }
            }
            if (_state.value.serverUp) _state.update { it.copy(serverUp = false) }
            delay(backoffMs)
            backoffMs = (backoffMs * 2).coerceAtMost(5_000L)
        }
    }

    /** What does not travel on the stream: the parameter table, the config and the models. */
    private suspend fun refreshStatic() {
        catching { ParamSpec.parse(api.params()) }.onSuccess { p -> _state.update { it.copy(params = p) } }
        catching { ConfigState.of(api.config()) }.onSuccess { c -> _state.update { it.copy(config = c) } }
        refreshModels()
    }

    suspend fun refreshModels() {
        withContext(Dispatchers.IO) {
            catching { ModelsList.of(api.models()) }.onSuccess { m -> _state.update { it.copy(models = m) } }
        }
    }

    private fun onEvent(ev: SseEvent) {
        val o = runCatching { JSONObject(ev.data) }.getOrNull() ?: return
        when (ev.event) {
            "state" -> {
                val info = ServerInfo.of(o)
                val before = _state.value.info
                _state.update { it.copy(info = info) }
                if (info.ready && (before?.model?.path != info.model?.path || before?.model?.loadS != info.model?.loadS)) {
                    // A new session: a fresh metrics file for it, and the models list may have changed.
                    recorder?.sessionOpened(info, _state.value.config)
                    scope.launch { refreshModels() }
                }
                if (info.state == "empty" || info.state == "error") recorder?.close()
                if (info.generating) {
                    cancelIdleUnload()
                } else if (info.ready) {
                    scheduleIdleUnload()
                }
            }
            "config" -> _state.update { it.copy(config = ConfigState.of(o)) }
            "token" -> {
                val t = _state.value.live.copy().applyToken(o)
                // The threads the figures are read against belong to the session that produced them.
                _state.value.info?.model?.let { m ->
                    t.computeThreads = m.computeThreads
                    t.busyThreads = m.busyThreads
                }
                if (t.step <= 1) t.avgTokensPerSecond = -1.0
                recorder?.token(o)
                val now = System.currentTimeMillis()
                val temp = if (now - lastThermalSampleMs > 1500) {
                    lastThermalSampleMs = now
                    thermal?.sampleCelsius()
                } else null
                _state.update { it.copy(live = t, cpuTempC = temp ?: it.cpuTempC) }
            }
            "done" -> {
                val t = _state.value.live.copy().applyDone(o)
                recorder?.done(o)
                _state.update { it.copy(live = t, lastDone = o) }
            }
            else -> {} // downloads are the app's own, not the server's
        }
    }

    /** The live panel starts from zero at each generation. */
    fun resetLive() {
        _state.update { it.copy(live = Telemetry(), lastDone = null) }
    }

    // ── calls ──

    suspend fun load(modelPath: String?): Result<Unit> = call {
        cancelIdleUnload()
        api.load(modelPath)
    }

    suspend fun unload(): Result<Unit> = call { api.unload() }
    suspend fun cancel(): Result<Unit> = call { api.cancel() }

    suspend fun setConfig(values: Map<String, Any?>, reset: List<String> = emptyList()): Result<ConfigState> =
        withContext(Dispatchers.IO) {
            catching {
                val v = JSONObject()
                for ((k, x) in values) v.put(k, x)
                ConfigState.of(api.updateConfig(v, reset)).also { c -> _state.update { it.copy(config = c) } }
            }
        }

    suspend fun setAutoPlan(on: Boolean): Result<ConfigState> = withContext(Dispatchers.IO) {
        catching { ConfigState.of(api.updateConfig(JSONObject(), emptyList(), on)).also { c -> _state.update { it.copy(config = c) } } }
    }

    suspend fun plan(measure: Boolean): Result<PlanResult> = withContext(Dispatchers.IO) {
        catching { PlanResult.of(api.plan(measure)) }
    }

    suspend fun applyPlan(): Result<ConfigState> = withContext(Dispatchers.IO) {
        catching { ConfigState.of(api.applyPlan()).also { c -> _state.update { it.copy(config = c) } } }
    }

    /** The client for a chat stream: the chat runs it on its own thread and owns the connection. */
    fun api(): Api = api

    private suspend fun call(block: () -> Unit): Result<Unit> = withContext(Dispatchers.IO) { catching(block) }

    // ── idle unload: a loaded model holds model-sized memory, and a phone has other things to do ──

    private fun scheduleIdleUnload() {
        cancelIdleUnload()
        idleUnload = scope.launch {
            delay(AppPrefs.IDLE_UNLOAD_MINUTES * 60_000L)
            val s = _state.value
            if (s.ready && !s.generating) catching { api.unload() }
        }
    }

    private fun cancelIdleUnload() {
        idleUnload?.cancel()
        idleUnload = null
    }
}

/**
 * [runCatching] that lets cancellation through: a coroutine that was cancelled must stop, not read
 * its cancellation as a failed call and carry on.
 */
internal inline fun <T> catching(block: () -> T): Result<T> = try {
    Result.success(block())
} catch (c: CancellationException) {
    throw c
} catch (t: Throwable) {
    Result.failure(t)
}
