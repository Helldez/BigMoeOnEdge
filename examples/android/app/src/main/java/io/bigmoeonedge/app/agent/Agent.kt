package io.bigmoeonedge.app.agent

import android.content.Context
import io.bigmoeonedge.app.data.ConversationStore
import io.bigmoeonedge.app.engine.ApiException
import io.bigmoeonedge.app.engine.Engine
import io.bigmoeonedge.app.engine.catching
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File

/** What a new chat starts with, until it is sent and becomes the server's conversation. */
data class Draft(val mode: AgentMode = AgentMode.ASK, val route: String = ConversationSummary.ROUTE_LOCAL, val workspace: String = "")

/** The agent as the screens observe it. */
data class AgentUi(
    val conversations: List<ConversationSummary> = emptyList(),
    /** The open conversation, or null for a new chat not sent yet. */
    val open: ConversationSummary? = null,
    val items: List<AgentItem> = emptyList(),
    val opening: Boolean = false,
    val draft: Draft = Draft(),
    val workspaces: List<Workspace> = emptyList(),
    val providers: List<Provider> = emptyList(),
    /** Null until a screen asked for them. */
    val skills: List<Skill>? = null,
    val tasks: List<AgentTask>? = null,
    /** A send on its way to the server (the `202` not back yet). */
    val sending: Boolean = false,
    val error: String? = null,
    val errorCode: String = "",
    /** The server answered the agent's routes with 404: a build without the agent. */
    val unsupported: Boolean = false,
) {
    val mode get() = open?.mode ?: draft.mode
    val route get() = open?.route ?: draft.route
    val workspaceId get() = open?.workspace ?: draft.workspace
    val workspace get() = workspaces.firstOrNull { it.id == workspaceId }

    /** A run is going on in the open conversation: Stop instead of Send. */
    val running get() = open?.busy == true || items.any { it.streaming }
    val routes get() = routesOf(providers)
}

/**
 * The agent's conversations, held by the server (docs/agent.md): this object lists them, opens one,
 * sends and answers, and follows the `conversation`, `item`, `delta` and `task` events the engine's
 * one event stream carries. Nothing of a conversation is kept on the phone: a run goes on in the
 * server process whether a screen is open or not, and the transcript is rebuilt from the server.
 */
object Agent {
    private val _state = MutableStateFlow(AgentUi())
    val state: StateFlow<AgentUi> = _state.asStateFlow()

    /** A conversation a notification asked to open: the chat screen comes to the front for it. */
    private val _openRequest = MutableStateFlow<String?>(null)
    val openRequest: StateFlow<String?> = _openRequest.asStateFlow()

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    @Volatile private var appContext: Context? = null
    @Volatile private var migrated = false

    /** Whether a screen of the app is on the display: a notification is for when none is. */
    @Volatile var foreground = false

    val current: AgentUi get() = _state.value

    fun init(ctx: Context) {
        appContext = ctx.applicationContext
    }

    /** The folder the app's own conversations were kept in before the server held them. */
    private fun legacyDir(ctx: Context) = File(ctx.filesDir, "conversations")

    // ── what the engine's event stream hands over ──

    /** The server is up (again): lists, the conversations kept on the phone carried over once, the open one re-read. */
    fun onServerUp() {
        scope.launch {
            migrateOnce()
            refreshConversations()
            refreshWorkspaces()
            refreshProviders()
            _state.value.open?.id?.let { reopen(it) }
        }
    }

    fun onEvent(event: String, data: JSONObject) {
        when (event) {
            "conversation" -> {
                val id = data.optString("id")
                val before = _state.value.conversations.firstOrNull { it.id == id }?.state
                    ?: _state.value.open?.takeIf { it.id == id }?.state
                _state.update { s ->
                    val list = ItemReducer.conversation(s.conversations, data)
                    val row = list.firstOrNull { it.id == id }
                    s.copy(conversations = list, open = if (s.open?.id == id && row != null) s.open!!.copy(title = row.title, state = row.state, updated = row.updated) else s.open)
                }
                val after = _state.value.conversations.firstOrNull { it.id == id }
                if (after != null) notifyOnChange(after, before)
            }
            "item", "delta" -> _state.update { s ->
                s.copy(items = ItemReducer.apply(s.open?.id, s.items, event, data))
            }
            "task" -> {
                val t = AgentTask.of(data)
                _state.update { s ->
                    val list = s.tasks ?: return@update s
                    val i = list.indexOfFirst { it.id == t.id }
                    s.copy(tasks = if (i < 0) list + t else list.toMutableList().also { it[i] = t })
                }
            }
        }
    }

    private fun notifyOnChange(c: ConversationSummary, before: String?) {
        if (foreground) return
        val ctx = appContext ?: return
        val title = c.title.ifEmpty { "A conversation" }
        when {
            c.state == ConversationSummary.STATE_WAITING && before != ConversationSummary.STATE_WAITING ->
                AgentNotifier.post(ctx, c.id, "Approval needed", "$title is waiting for you to approve a change.")
            (before == ConversationSummary.STATE_RUNNING || before == ConversationSummary.STATE_WAITING) &&
                (c.state == ConversationSummary.STATE_IDLE || c.state == ConversationSummary.STATE_ERROR) ->
                AgentNotifier.post(
                    ctx, c.id,
                    if (c.state == ConversationSummary.STATE_ERROR) "Run stopped with an error" else "Run finished",
                    title,
                )
        }
    }

    // ── conversations ──

    suspend fun refreshConversations() = withContext(Dispatchers.IO) {
        try {
            val list = ConversationSummary.list(Engine.api().conversations())
            _state.update { it.copy(conversations = list, unsupported = false) }
        } catch (e: ApiException) {
            if (e.status == 404) _state.update { it.copy(unsupported = true) }
        } catch (_: Throwable) {
        }
    }

    /** The conversations this app kept on the phone, once per launch while any are left. */
    private fun migrateOnce() {
        if (migrated) return
        val ctx = appContext ?: return
        val dir = legacyDir(ctx)
        if (!dir.isDirectory) {
            migrated = true
            return
        }
        val r = Migration.run(StoreConversations(ConversationStore(dir)), Engine.api())
        if (r.error == null) migrated = true
        if (r.kept == 0 && r.error == null) dir.listFiles()?.isEmpty()?.let { empty -> if (empty) dir.delete() }
    }

    fun newConversation() {
        _state.update { it.copy(open = null, items = emptyList(), error = null, errorCode = "", draft = it.draft.copy(mode = it.mode, route = it.route, workspace = it.workspaceId)) }
    }

    fun open(id: String) {
        _state.update { it.copy(opening = true, error = null, errorCode = "") }
        scope.launch { reopen(id) }
    }

    fun consumeOpenRequest(): String? = _openRequest.value.also { _openRequest.value = null }

    /** A notification's tap: the conversation it named, opened on the chat screen. */
    fun requestOpen(id: String) {
        _openRequest.value = id
        open(id)
    }

    private fun reopen(id: String) {
        try {
            val c = ConversationFull.of(Engine.api().conversation(id))
            _state.update { it.copy(open = c.summary, items = c.items, opening = false) }
        } catch (t: Throwable) {
            _state.update { it.copy(opening = false, error = t.message ?: "The conversation could not be read.", errorCode = (t as? ApiException)?.code.orEmpty()) }
        }
    }

    fun delete(id: String) {
        scope.launch {
            catching { Engine.api().deleteConversation(id) }.onSuccess {
                _state.update { s ->
                    val wasOpen = s.open?.id == id
                    s.copy(conversations = s.conversations.filter { it.id != id }, open = if (wasOpen) null else s.open, items = if (wasOpen) emptyList() else s.items)
                }
            }.onFailure { fail(it) }
        }
    }

    fun rename(id: String, title: String) = patch(id, JSONObject().put("title", title))

    fun setMode(m: AgentMode) {
        val open = _state.value.open
        if (open == null) _state.update { it.copy(draft = it.draft.copy(mode = m)) } else patch(open.id, JSONObject().put("mode", m.wire))
    }

    fun setRoute(route: String) {
        val open = _state.value.open
        if (open == null) _state.update { it.copy(draft = it.draft.copy(route = route)) } else patch(open.id, JSONObject().put("route", route))
    }

    fun setWorkspace(id: String) {
        val open = _state.value.open
        if (open == null) _state.update { it.copy(draft = it.draft.copy(workspace = id)) }
        else patch(open.id, JSONObject().put("workspace", id.ifEmpty { JSONObject.NULL }))
    }

    private fun patch(id: String, body: JSONObject) {
        scope.launch {
            catching { ConversationSummary.of(Engine.api().patchConversation(id, body)) }.onSuccess { c ->
                _state.update { s ->
                    val merged = if (c.id.isEmpty()) null else c
                    s.copy(
                        open = if (s.open?.id == id) merged ?: s.open else s.open,
                        conversations = s.conversations.map { if (it.id == id && merged != null) merged else it },
                    )
                }
                if (c.id.isEmpty()) refreshConversations()
            }.onFailure { fail(it) }
        }
    }

    /** Send [text] to the open conversation, making it first when this is a new chat. */
    fun send(text: String) {
        val prompt = text.trim()
        val s = _state.value
        if (prompt.isEmpty() || s.sending || s.running) return
        if (s.route == ConversationSummary.ROUTE_LOCAL && !Engine.current.ready) {
            val up = Engine.current.serverUp
            _state.update {
                it.copy(
                    error = if (up) "Load a model first, from Models, or pick a provider's model." else "The engine is not running.",
                    errorCode = if (up) ApiException.CODE_NO_MODEL else "",
                )
            }
            return
        }
        _state.update { it.copy(sending = true, error = null, errorCode = "") }
        scope.launch {
            try {
                val api = Engine.api()
                var open = _state.value.open
                if (open == null) {
                    val d = _state.value.draft
                    val body = JSONObject().put("mode", d.mode.wire).put("route", d.route)
                        .put("title", io.bigmoeonedge.app.data.Conversation.titleFor(prompt))
                    if (d.workspace.isNotEmpty()) body.put("workspace", d.workspace)
                    open = ConversationSummary.of(api.createConversation(body))
                    val made = open
                    _state.update { it.copy(open = made, items = emptyList(), conversations = listOf(made) + it.conversations.filter { c -> c.id != made.id }) }
                }
                val r = api.sendMessage(open.id, prompt)
                r.optJSONObject("item")?.let { o -> _state.update { it.copy(items = ItemReducer.upsert(it.items, AgentItem.of(o))) } }
                _state.update { it.copy(sending = false) }
            } catch (t: Throwable) {
                fail(t)
                _state.update { it.copy(sending = false) }
            }
        }
    }

    /** Drop [item] (the person's message) and everything after it, then send [text] in its place. */
    fun editAndResend(item: String, text: String) {
        val open = _state.value.open ?: return
        val t = text.trim().ifEmpty { return }
        _state.update { s ->
            val i = s.items.indexOfFirst { it.id == item }
            s.copy(items = if (i < 0) s.items else s.items.take(i), error = null, errorCode = "")
        }
        scope.launch { catching { Engine.api().editMessage(open.id, item, t) }.onFailure { fail(it); reopen(open.id) } }
    }

    fun stop() {
        val open = _state.value.open ?: return
        scope.launch { catching { Engine.api().stopConversation(open.id) }.onFailure { fail(it) } }
    }

    fun answer(item: String, approve: Boolean) {
        val open = _state.value.open ?: return
        scope.launch { catching { Engine.api().approval(open.id, item, approve) }.onFailure { fail(it) } }
    }

    fun clearError() = _state.update { it.copy(error = null, errorCode = "") }

    fun file(path: String): Result<Pair<ByteArray, String>> {
        val open = _state.value.open ?: return Result.failure(IllegalStateException("No conversation is open."))
        return catching { Engine.api().conversationFile(open.id, path) }
    }

    private fun fail(t: Throwable) {
        _state.update { it.copy(error = t.message ?: t.javaClass.simpleName, errorCode = (t as? ApiException)?.code.orEmpty()) }
    }

    // ── workspaces ──

    suspend fun refreshWorkspaces() = withContext(Dispatchers.IO) {
        catching { Workspace.list(Engine.api().workspaces()) }.onSuccess { w -> _state.update { it.copy(workspaces = w) } }
    }

    /** A folder in the app's own storage, made and handed to the server as a workspace. */
    suspend fun createWorkspace(name: String): Result<Workspace> = withContext(Dispatchers.IO) {
        val ctx = appContext ?: return@withContext Result.failure(IllegalStateException("not started"))
        catching {
            val dir = LocalFolders.create(ctx, name)
            Workspace.of(Engine.api().addWorkspace(dir.absolutePath, name.trim()))
        }.also { refreshWorkspaces() }
    }

    /** The server forgets the folder; the files stay where they are. */
    suspend fun forgetWorkspace(id: String): Result<Unit> = withContext(Dispatchers.IO) {
        catching { Engine.api().removeWorkspace(id); Unit }.also {
            refreshWorkspaces()
            if (_state.value.draft.workspace == id) _state.update { s -> s.copy(draft = s.draft.copy(workspace = "")) }
        }
    }

    // ── providers, skills, tasks ──

    suspend fun refreshProviders() = withContext(Dispatchers.IO) {
        catching { Provider.list(Engine.api().providers()) }.onSuccess { p -> _state.update { it.copy(providers = p) } }
    }

    suspend fun saveProvider(id: String, name: String, baseUrl: String, models: List<String>, key: String): Result<Unit> = withContext(Dispatchers.IO) {
        catching { Engine.api().saveProvider(id, name, baseUrl, models, key); Unit }.also { refreshProviders() }
    }

    suspend fun deleteProvider(id: String): Result<Unit> = withContext(Dispatchers.IO) {
        catching { Engine.api().deleteProvider(id); Unit }.also { refreshProviders() }
    }

    suspend fun refreshSkills(): Result<Unit> = withContext(Dispatchers.IO) {
        catching { Skill.list(Engine.api().skills()) }.onSuccess { k -> _state.update { it.copy(skills = k) } }.map { }
    }

    suspend fun saveSkill(name: String, description: String, body: String, isNew: Boolean): Result<Unit> = withContext(Dispatchers.IO) {
        catching { Engine.api().saveSkill(name, description, body, isNew); Unit }.also { refreshSkills() }
    }

    suspend fun enableSkill(name: String, on: Boolean): Result<Unit> = withContext(Dispatchers.IO) {
        _state.update { s -> s.copy(skills = s.skills?.map { if (it.name == name) it.copy(enabled = on) else it }) }
        catching { Engine.api().enableSkill(name, on); Unit }.also { refreshSkills() }
    }

    suspend fun deleteSkill(name: String): Result<Unit> = withContext(Dispatchers.IO) {
        catching { Engine.api().deleteSkill(name); Unit }.also { refreshSkills() }
    }

    suspend fun refreshTasks(): Result<Unit> = withContext(Dispatchers.IO) {
        catching { AgentTask.list(Engine.api().tasks()) }.onSuccess { t -> _state.update { it.copy(tasks = t) } }.map { }
    }

    suspend fun saveTask(t: AgentTask): Result<Unit> = withContext(Dispatchers.IO) {
        catching { Engine.api().saveTask(t.id, t.toJson()); Unit }.also { refreshTasks() }
    }

    suspend fun deleteTask(id: String): Result<Unit> = withContext(Dispatchers.IO) {
        catching { Engine.api().deleteTask(id); Unit }.also { refreshTasks() }
    }

    suspend fun runTask(id: String): Result<Unit> = withContext(Dispatchers.IO) {
        catching { Engine.api().runTask(id); Unit }
    }
}
