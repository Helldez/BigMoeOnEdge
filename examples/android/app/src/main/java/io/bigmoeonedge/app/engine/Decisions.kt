package io.bigmoeonedge.app.engine

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

/** A decision in flight or answered: the question, its options and what the model put on each. */
data class DecisionUi(
    val question: String,
    val options: List<String>,
    val running: Boolean = true,
    val scores: List<ChoiceScore> = emptyList(),
    val best: Int = -1,
    val metrics: String = "",
    val error: String? = null,
    val errorCode: String = "",
)

/**
 * Choose from options (docs/decide.md) beside the conversations. A decision is not a
 * conversation turn: the model picks one option from a single prefill and writes nothing, so its
 * answer is shown on the chat screen until dismissed and is never sent to the server's
 * conversation.
 */
object Decisions {
    private val _state = MutableStateFlow<DecisionUi?>(null)
    val state: StateFlow<DecisionUi?> = _state.asStateFlow()
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    fun choose(question: String, options: List<String>) {
        val q = question.trim()
        if (q.isEmpty() || options.size < 2 || _state.value?.running == true) return
        val engine = Engine.current
        if (!engine.ready) {
            _state.value = DecisionUi(
                q, options, running = false,
                error = if (engine.serverUp) "Load a model first, from Models." else "The engine is not running.",
                errorCode = if (engine.serverUp) ApiException.CODE_NO_MODEL else "",
            )
            return
        }
        _state.value = DecisionUi(q, options)
        Engine.resetLive()
        scope.launch {
            val next = try {
                val r = Decide.parse(Engine.api().decide(Decide.requestBody(q, options)), options)
                if (r.cancelled) null else DecisionUi(q, options, running = false, scores = r.scores, best = r.best, metrics = r.metrics)
            } catch (e: ApiException) {
                DecisionUi(q, options, running = false, error = e.message, errorCode = e.code)
            } catch (t: Throwable) {
                DecisionUi(q, options, running = false, error = t.message ?: "the connection to the engine was lost")
            }
            _state.value = next
        }
    }

    fun stop() {
        if (_state.value?.running != true) return
        scope.launch { runCatching { Engine.api().cancel() } }
    }

    fun dismiss() {
        if (_state.value?.running == true) return
        _state.value = null
    }
}
