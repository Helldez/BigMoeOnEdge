package io.bigmoeonedge.app.chat

import android.content.Context
import io.bigmoeonedge.app.data.Conversation
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

/** The chat as the screens observe it. */
data class ChatUi(
    val conversations: List<ConversationSummary> = emptyList(),
    /** The open conversation, or null for a new chat not sent yet. */
    val open: ConversationSummary? = null,
    val items: List<ChatItem> = emptyList(),
    val opening: Boolean = false,
    /** A send on its way to the server (the `202` not back yet). */
    val sending: Boolean = false,
    val error: String? = null,
    val errorCode: String = "",
    /** The server answered the conversation routes with 404: a build without them. */
    val unsupported: Boolean = false,
) {
    /** An answer is being written in the open conversation: Stop instead of Send. */
    val running get() = open?.busy == true || items.any { it.streaming }
}

/**
 * The conversations, held by the server (docs/agent.md) and plain chat, since the app starts its
 * server with `--chat-only`: this object lists them, opens one, sends, and follows the
 * `conversation`, `item` and `delta` events the engine's one event stream carries. Nothing of a
 * conversation is kept on the phone: an answer goes on in the server process whether a screen is
 * open or not, and the transcript is rebuilt from the server.
 */
object Chat {
    private val _state = MutableStateFlow(ChatUi())
    val state: StateFlow<ChatUi> = _state.asStateFlow()

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    @Volatile private var appContext: Context? = null
    @Volatile private var migrated = false

    fun init(ctx: Context) {
        appContext = ctx.applicationContext
    }

    /** The folder the app's own conversations were kept in before the server held them. */
    private fun legacyDir(ctx: Context) = File(ctx.filesDir, "conversations")

    // ── what the engine's event stream hands over ──

    /** The server is up (again): the conversations kept on the phone carried over once, the list and the open one re-read. */
    fun onServerUp() {
        scope.launch {
            migrateOnce()
            refreshConversations()
            _state.value.open?.id?.let { reopen(it) }
        }
    }

    fun onEvent(event: String, data: JSONObject) {
        when (event) {
            "conversation" -> _state.update { s ->
                val id = data.optString("id")
                val list = ItemReducer.conversation(s.conversations, data)
                val row = list.firstOrNull { it.id == id }
                s.copy(conversations = list, open = if (s.open?.id == id && row != null) s.open!!.copy(title = row.title, state = row.state, updated = row.updated) else s.open)
            }
            "item", "delta" -> _state.update { s ->
                s.copy(items = ItemReducer.apply(s.open?.id, s.items, event, data))
            }
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
        _state.update { it.copy(open = null, items = emptyList(), error = null, errorCode = "") }
    }

    fun open(id: String) {
        _state.update { it.copy(opening = true, error = null, errorCode = "") }
        scope.launch { reopen(id) }
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

    fun rename(id: String, title: String) {
        scope.launch {
            catching { ConversationSummary.of(Engine.api().patchConversation(id, JSONObject().put("title", title))) }.onSuccess { c ->
                if (c.id.isEmpty()) {
                    refreshConversations()
                    return@onSuccess
                }
                _state.update { s ->
                    s.copy(
                        open = if (s.open?.id == id) c else s.open,
                        conversations = s.conversations.map { if (it.id == id) c else it },
                    )
                }
            }.onFailure { fail(it) }
        }
    }

    /** Whether a message can go now; the reason in the error card when it cannot. */
    private fun canSend(): Boolean {
        if (Engine.current.ready) return true
        val up = Engine.current.serverUp
        _state.update {
            it.copy(
                error = if (up) "Load a model first, from Models." else "The engine is not running.",
                errorCode = if (up) ApiException.CODE_NO_MODEL else "",
            )
        }
        return false
    }

    /** Send [text] to the open conversation, making it first when this is a new chat. */
    fun send(text: String) {
        val prompt = text.trim()
        val s = _state.value
        if (prompt.isEmpty() || s.sending || s.running || !canSend()) return
        _state.update { it.copy(sending = true, error = null, errorCode = "") }
        scope.launch {
            try {
                val api = Engine.api()
                var open = _state.value.open
                if (open == null) {
                    open = ConversationSummary.of(api.createConversation(JSONObject().put("title", Conversation.titleFor(prompt))))
                    val made = open
                    _state.update { it.copy(open = made, items = emptyList(), conversations = listOf(made) + it.conversations.filter { c -> c.id != made.id }) }
                }
                val r = api.sendMessage(open.id, prompt)
                r.optJSONObject("item")?.let { o -> _state.update { it.copy(items = ItemReducer.upsert(it.items, ChatItem.of(o))) } }
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
        if (_state.value.running || !canSend()) return
        _state.update { s -> s.copy(items = ItemReducer.before(s.items, item), error = null, errorCode = "") }
        scope.launch { catching { Engine.api().editMessage(open.id, item, t) }.onFailure { fail(it); reopen(open.id) } }
    }

    /** Answer again: the last question, resent as it was, as the web UI does. */
    fun answerAgain() {
        val last = ItemReducer.lastUser(_state.value.items) ?: return
        editAndResend(last.id, last.text)
    }

    fun stop() {
        val open = _state.value.open ?: return
        scope.launch { catching { Engine.api().stopConversation(open.id) }.onFailure { fail(it) } }
    }

    fun clearError() = _state.update { it.copy(error = null, errorCode = "") }

    private fun fail(t: Throwable) {
        _state.update { it.copy(error = t.message ?: t.javaClass.simpleName, errorCode = (t as? ApiException)?.code.orEmpty()) }
    }
}
