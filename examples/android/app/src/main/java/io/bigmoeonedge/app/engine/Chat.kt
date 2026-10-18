package io.bigmoeonedge.app.engine

import android.content.Context
import io.bigmoeonedge.app.data.Conversation
import io.bigmoeonedge.app.data.ConversationMeta
import io.bigmoeonedge.app.data.ConversationStore
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import java.io.File

/** The chat as the screen observes it. */
data class ChatUi(
    val conversations: List<ConversationMeta> = emptyList(),
    val current: Conversation = Conversation.fresh(),
    /** The answer and the thinking span in flight; committed to [current] when the turn ends. */
    val answer: String = "",
    val reasoning: String = "",
    val streaming: Boolean = false,
    val error: String? = null,
    /** The error's code (docs/server-api.md), what the screen decides on; empty when it had none. */
    val errorCode: String = "",
) {
    /** The error is a full context: the fix is a new chat, and the screen says so. */
    val contextFull: Boolean get() = errorCode == ApiException.CODE_CONTEXT_FULL

    /** The error is a server without a model: the fix is on Models, and the screen links there. */
    val noModel: Boolean get() = errorCode == ApiException.CODE_NO_MODEL

    /** The last message is the person's, unanswered: a turn failed, and it can be sent again as it is. */
    val canRetry: Boolean get() = !streaming && current.messages.lastOrNull()?.role == "user"
}

/**
 * Conversations and the turn in flight. The server keeps no history: every request carries the
 * whole conversation and the engine reuses the leading tokens its KV cache already holds, so a
 * follow-up costs its own message and switching conversations costs a full prefill. What is kept
 * here is saved on the device, one file per conversation, and survives a restart.
 */
object Chat {
    private val _state = MutableStateFlow(ChatUi())
    val state: StateFlow<ChatUi> = _state.asStateFlow()

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var store: ConversationStore? = null
    @Volatile private var stopRequested = false

    fun init(ctx: Context) {
        if (store != null) return
        val s = ConversationStore(File(ctx.applicationContext.filesDir, "conversations"))
        store = s
        _state.update { it.copy(conversations = s.list()) }
    }

    fun newConversation() {
        if (_state.value.streaming) return
        _state.update { it.copy(current = Conversation.fresh(), answer = "", reasoning = "", error = null, errorCode = "") }
    }

    fun select(id: String) {
        if (_state.value.streaming) return
        val c = store?.load(id) ?: return
        _state.update { it.copy(current = c, answer = "", reasoning = "", error = null, errorCode = "") }
    }

    fun delete(id: String) {
        store?.delete(id)
        _state.update {
            val cur = if (it.current.id == id && !it.streaming) Conversation.fresh() else it.current
            it.copy(conversations = store?.list().orEmpty(), current = cur)
        }
    }

    fun clearError() = _state.update { it.copy(error = null, errorCode = "") }

    /** Send [text] as the next user turn of the current conversation. */
    fun send(text: String) {
        val prompt = text.trim()
        if (prompt.isEmpty() || _state.value.streaming) return
        val engine = Engine.current
        if (!engine.ready) {
            notReady(engine)
            return
        }
        val before = _state.value.current
        val withUser = before.copy(
            title = before.title.ifEmpty { Conversation.titleFor(prompt) },
            messages = before.messages + ChatMessage("user", prompt),
            updatedAt = System.currentTimeMillis(),
            model = engine.info?.model?.name ?: before.model,
        )
        persist(withUser)
        _state.update { it.copy(current = withUser, answer = "", reasoning = "", streaming = true, error = null, errorCode = "") }
        run(withUser)
    }

    /**
     * A decision: the model picks one of [options] for [question] from one prefill and no decode.
     * Shown in the conversation as the question with its lettered options, then the options with
     * the probability the model put on each; never sent back to the model as history.
     */
    fun choose(question: String, options: List<String>) {
        val q = question.trim()
        if (q.isEmpty() || options.size < 2 || _state.value.streaming) return
        val engine = Engine.current
        if (!engine.ready) {
            notReady(engine)
            return
        }
        val before = _state.value.current
        val withUser = before.copy(
            title = before.title.ifEmpty { Conversation.titleFor(q) },
            messages = before.messages + ChatMessage("user", Decide.userText(q, options)),
            updatedAt = System.currentTimeMillis(),
            model = engine.info?.model?.name ?: before.model,
        )
        persist(withUser)
        _state.update { it.copy(current = withUser, answer = "", reasoning = "", streaming = true, error = null, errorCode = "") }
        stopRequested = false
        Engine.resetLive()
        scope.launch {
            var error: String? = null
            var errorCode = ""
            var answer: ChatMessage? = null
            try {
                val r = Decide.parse(Engine.api().decide(Decide.requestBody(q, options)), options)
                if (!r.cancelled) answer = ChatMessage("assistant", "", "", r.metrics, r.scores, r.best)
            } catch (e: ApiException) {
                error = e.message
                errorCode = e.code
            } catch (t: Throwable) {
                error = t.message ?: "the connection to the engine was lost"
            }
            // A decision's question is not a conversation turn: without its answer it would be sent
            // as history with the next message, so a failed or cancelled one leaves none behind.
            val committed = answer?.let { withUser.copy(messages = withUser.messages + it, updatedAt = System.currentTimeMillis()) } ?: before
            persist(committed)
            _state.update { it.copy(current = committed, streaming = false, error = error, errorCode = errorCode) }
        }
    }

    /** Drop the last answer and ask again: the KV holds the prompt, so only the answer is paid. */
    fun regenerate() {
        val cur = _state.value.current
        if (_state.value.streaming || cur.messages.lastOrNull()?.role != "assistant" || cur.messages.last().choices != null) return
        if (!Engine.current.ready) {
            notReady(Engine.current)
            return
        }
        val trimmed = cur.copy(messages = cur.messages.dropLast(1), updatedAt = System.currentTimeMillis())
        persist(trimmed)
        _state.update { it.copy(current = trimmed, answer = "", reasoning = "", streaming = true, error = null, errorCode = "") }
        run(trimmed)
    }

    /** Send the conversation again as it stands, after a failed turn left the person's message last. */
    fun retry() {
        val s = _state.value
        if (!s.canRetry) return
        if (!Engine.current.ready) {
            notReady(Engine.current)
            return
        }
        _state.update { it.copy(answer = "", reasoning = "", streaming = true, error = null, errorCode = "") }
        run(s.current)
    }

    /** Stop the turn in flight; what streamed so far is kept as the answer. */
    fun stop() {
        if (!_state.value.streaming) return
        stopRequested = true
        scope.launch { runCatching { Engine.api().cancel() } }
    }

    private fun run(conversation: Conversation) {
        stopRequested = false
        Engine.resetLive()
        scope.launch {
            val content = StringBuilder()
            val reasoning = StringBuilder()
            var finish: String? = null
            var summary: org.json.JSONObject? = null
            var error: String? = null
            var errorCode = ""
            try {
                val body = ChatProtocol.requestBody(conversation.messages)
                Engine.api().chatStream(body, { data ->
                    val chunk = ChatProtocol.parseChunk(data) ?: return@chatStream
                    if (chunk.error != null) {
                        error = chunk.error
                        errorCode = chunk.errorCode
                    }
                    if (chunk.reset) {
                        content.setLength(0)
                        reasoning.setLength(0)
                    }
                    chunk.content?.let { content.append(it) }
                    chunk.reasoning?.let { reasoning.append(it) }
                    chunk.finish?.let { finish = it }
                    chunk.summary?.let { summary = it }
                    _state.update { it.copy(answer = content.toString(), reasoning = reasoning.toString()) }
                }, { true })
            } catch (e: ApiException) {
                error = e.message
                errorCode = e.code
            } catch (t: Throwable) {
                error = if (stopRequested) null else (t.message ?: "the connection to the engine was lost")
            }
            val nCtx = Engine.current.info?.model?.nCtx ?: 0
            val cancelled = finish == "cancelled" || stopRequested
            val metrics = summary?.let { ChatProtocol.turnMetrics(it, nCtx, cancelled) }.orEmpty()
            val answer = content.toString()
            // A turn that produced nothing and failed keeps the user's message, so it can be sent
            // again after the fix (a new chat, a loaded model); an empty cancelled turn is dropped.
            val committed = if (answer.isNotEmpty() || (error == null && !cancelled)) {
                conversation.copy(
                    messages = conversation.messages + ChatMessage("assistant", answer, reasoning.toString(), metrics),
                    updatedAt = System.currentTimeMillis(),
                )
            } else conversation
            if (committed !== conversation) persist(committed)
            _state.update {
                // The person may have moved on to another conversation only when not streaming,
                // which cannot happen here: the current one is the one that ran.
                it.copy(current = committed, answer = "", reasoning = "", streaming = false, error = error, errorCode = errorCode)
            }
        }
    }

    /** Nothing to send to: said with the code the server itself would answer, so one rule reads both. */
    private fun notReady(engine: EngineUi) {
        _state.update {
            if (engine.serverUp) it.copy(error = "Load a model first, from Models.", errorCode = ApiException.CODE_NO_MODEL)
            else it.copy(error = "The engine is not running.", errorCode = "")
        }
    }

    private fun persist(c: Conversation) {
        val s = store ?: return
        runCatching { s.save(c) }
        _state.update { it.copy(conversations = s.list()) }
    }
}
