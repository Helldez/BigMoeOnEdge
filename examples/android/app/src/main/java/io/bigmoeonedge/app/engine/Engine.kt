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
) {
    companion object {
        fun of(o: JSONObject?): ModelInfo? {
            if (o == null) return null
            return ModelInfo(
                o.optString("name"), o.optString("path"), o.optString("arch"), o.optInt("n_ctx"),
                o.optInt("n_expert_used"), o.optString("think_ctl", "template"), o.optDouble("load_s", 0.0),
                o.optBoolean("streaming", true),
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
    fun int(key: String, fallback: Int): Int = (values[key] as? Number)?.toInt() ?: fallback
    fun bool(key: String, fallback: Boolean): Boolean = (values[key] as? Boolean) ?: fallback
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
    val bytes: Long,
    val shards: Int,
    val arch: String,
    val streamable: Boolean,
    val complete: Boolean,
)

/** `GET /api/models`: the server's directory, and the catalog with the server's view of each entry. */
data class ModelsList(val modelsDir: String, val local: List<LocalModel>, val catalog: List<CatalogEntry>) {
    companion object {
        fun of(o: JSONObject): ModelsList {
            val local = o.optJSONArray("local")?.let { a ->
                (0 until a.length()).map { i ->
                    val m = a.getJSONObject(i)
                    LocalModel(
                        m.optString("name"), m.optString("path"), m.optLong("bytes"), m.optInt("shards", 1),
                        m.optString("arch"), m.optBoolean("streamable", false), m.optBoolean("complete", true),
                    )
                }
            }.orEmpty()
            val catalog = o.optJSONArray("catalog")?.let { a ->
                (0 until a.length()).mapNotNull { i -> CatalogEntry.of(a.getJSONObject(i)) }
            }.orEmpty()
            return ModelsList(o.optString("models_dir"), local, catalog)
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
 * calls the screens make. The server runs in [EngineService]'s process; this object lives in the
 * app's, connects to loopback and keeps reconnecting, so a screen never has to know whether the
 * server is up, only what the state says.
 *
 * Nothing here knows an engine parameter by name beyond the two the chat itself needs: the
 * settings screen renders whatever `GET /api/params` describes.
 */
object Engine {
    private val _state = MutableStateFlow(EngineUi())
    val state: StateFlow<EngineUi> = _state.asStateFlow()

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    @Volatile private var api = Api(ServerProcess.baseUrl())
    private var connector: Job? = null
    private var idleUnload: Job? = null
    private var thermal: Thermal? = null
    private var recorder: MetricsRecorder? = null
    private var appContext: Context? = null
    @Volatile private var lastThermalSampleMs = 0L

    val current: EngineUi get() = _state.value

    /** Start the service (and so the server) if it is not running, and keep the state following it. */
    fun start(ctx: Context) {
        val app = ctx.applicationContext
        appContext = app
        if (thermal == null) thermal = Thermal(app)
        if (recorder == null) recorder = MetricsRecorder(app)
        ContextCompat.startForegroundService(app, Intent(app, EngineService::class.java).setAction(EngineService.ACTION_START))
        if (connector == null) connector = scope.launch { connectForever() }
    }

    /** Stop the server (and free the model). The next [start] brings it back. */
    fun stop(ctx: Context) {
        val app = ctx.applicationContext
        app.startService(Intent(app, EngineService::class.java).setAction(EngineService.ACTION_STOP))
    }

    /** Apply a changed sharing setting: the server has to be started again with the other address. */
    fun restart(ctx: Context) {
        stop(ctx)
        scope.launch {
            delay(800)
            start(ctx)
        }
    }

    // ── what the service reports ──

    internal fun serverStarting(share: Boolean, apiKey: String) {
        api = Api(ServerProcess.baseUrl(), if (share) apiKey else null)
        _state.update { it.copy(serverError = null, shared = share) }
    }

    internal fun serverStopping() {
        recorder?.close()
        _state.update { it.copy(serverUp = false, serverError = "The engine was stopped.", info = null) }
    }

    internal fun serverExited(code: Int, stderrTail: String) {
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
            try {
                api.events(
                    onEvent = { ev ->
                        if (!_state.value.serverUp) {
                            _state.update { it.copy(serverUp = true, serverError = null) }
                            backoffMs = 500L
                            scope.launch { refreshStatic() }
                        }
                        onEvent(ev)
                    },
                    keepGoing = { true },
                )
            } catch (t: Throwable) {
                // Not up yet, or gone: say why in the status line, and try again. A refusal that
                // is not a closed port (a key, a policy) is exactly what a person needs to read.
                val why = t.message?.takeIf { it.isNotBlank() && !it.contains("ECONNREFUSED") && !it.contains("Connection refused") }
                if (why != null) _state.update { it.copy(serverError = "Engine not reachable: $why") }
            }
            if (_state.value.serverUp) _state.update { it.copy(serverUp = false) }
            delay(backoffMs)
            backoffMs = (backoffMs * 2).coerceAtMost(5_000L)
        }
    }

    /** What does not travel on the stream: the parameter table, the config and the models. */
    private suspend fun refreshStatic() {
        runCatching { ParamSpec.parse(api.params()) }.onSuccess { p -> _state.update { it.copy(params = p) } }
        runCatching { ConfigState.of(api.config()) }.onSuccess { c -> _state.update { it.copy(config = c) } }
        refreshModels()
    }

    suspend fun refreshModels() {
        runCatching { ModelsList.of(api.models()) }.onSuccess { m -> _state.update { it.copy(models = m) } }
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
            runCatching {
                val v = JSONObject()
                for ((k, x) in values) v.put(k, x)
                ConfigState.of(api.updateConfig(v, reset)).also { c -> _state.update { it.copy(config = c) } }
            }
        }

    suspend fun setAutoPlan(on: Boolean): Result<ConfigState> = withContext(Dispatchers.IO) {
        runCatching { ConfigState.of(api.updateConfig(JSONObject(), emptyList(), on)).also { c -> _state.update { it.copy(config = c) } } }
    }

    suspend fun plan(measure: Boolean): Result<PlanResult> = withContext(Dispatchers.IO) {
        runCatching { PlanResult.of(api.plan(measure)) }
    }

    suspend fun applyPlan(): Result<ConfigState> = withContext(Dispatchers.IO) {
        runCatching { ConfigState.of(api.applyPlan()).also { c -> _state.update { it.copy(config = c) } } }
    }

    /** The client for a chat stream: the chat runs it on its own thread and owns the connection. */
    fun api(): Api = api

    private suspend fun call(block: () -> Unit): Result<Unit> = withContext(Dispatchers.IO) { runCatching(block) }

    // ── idle unload: a loaded model holds model-sized memory, and a phone has other things to do ──

    private fun scheduleIdleUnload() {
        cancelIdleUnload()
        idleUnload = scope.launch {
            delay(AppPrefs.IDLE_UNLOAD_MINUTES * 60_000L)
            val s = _state.value
            if (s.ready && !s.generating) runCatching { api.unload() }
        }
    }

    private fun cancelIdleUnload() {
        idleUnload?.cancel()
        idleUnload = null
    }
}
