package io.bigmoeonedge.example

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update

/**
 * Where the engine is in its lifecycle. Unlike the old per-run boolean pair, a session process
 * outlives a single generation: it loads once (LOADING), then sits READY between prompts, and
 * flips to GENERATING only while a prompt is being answered.
 */
enum class EngineState { IDLE, LOADING, READY, GENERATING, ERROR }

/**
 * One committed message in the multi-turn transcript. metrics is a compact per-turn line.
 * reasoning is the model's thinking span (assistant turns only), shown as a collapsible block
 * above the answer; empty when the model did not reason.
 */
data class ChatTurn(val role: String, val text: String, val metrics: String = "", val reasoning: String = "")

/** Immutable snapshot of the session + current generation, observed by the Compose UI. */
data class UiState(
    val state: EngineState = EngineState.IDLE,
    val telemetry: Telemetry = Telemetry(),
    val answer: String = "",
    val reasoning: String = "",      // in-flight thinking span; streams before the answer while the model reasons
    val summary: String = "",
    val error: String? = null,
    val ioMode: String? = null,     // effective read mode reported by the engine (direct / buffered)
    val cpuTempC: Double? = null,   // SoC/CPU temperature (°C), sampled while generating (battery fallback)
    val sessionSig: String? = null, // signature of the loaded session (AppSettings.sessionSignature)
    // How the loaded model can honour "Thinking off", reported once at BMOE_READY: "template" (its
    // chat template reads the flag), "prefill" (it does not, so the engine closes the reasoning span
    // in the prompt), or "none" (neither — the model always reasons, and the switch is hidden rather
    // than left there doing nothing). Null until a session reports it. See docs/telemetry.md.
    val thinkControl: String? = null,
    // Experts the loaded model routes per token, from BMOE_READY. Null = nothing loaded yet,
    // 0 = not MoE. Settings needs it because "Drop cold experts" is a fraction of 1/top-k, so the
    // same percentage means something very different on a narrow routing.
    val nExpertUsed: Int? = null,
    val transcript: List<ChatTurn> = emptyList(), // committed turns; the in-flight answer is `answer`
    val streaming: Boolean = true,  // is the loaded session using the MoE streamer (vs mmap baseline)?
) {
    val loading get() = state == EngineState.LOADING
    val generating get() = state == EngineState.GENERATING
    val ready get() = state == EngineState.READY
    val busy get() = state == EngineState.LOADING || state == EngineState.GENERATING
}

/**
 * Single source of truth shared between the RunService (writer) and the UI (reader). The service
 * pushes updates as the session process reports progress; the UI collects the StateFlow. One
 * session at a time, one generation at a time within it, so a single flow is enough.
 */
object RunBus {
    private val _state = MutableStateFlow(UiState())
    val state: StateFlow<UiState> = _state.asStateFlow()

    /** Reset the per-generation fields for a new prompt, preserving session state and signature. */
    fun resetGeneration() = _state.update {
        it.copy(telemetry = Telemetry(), answer = "", reasoning = "", summary = "", error = null)
    }

    fun setState(s: EngineState) = _state.update { it.copy(state = s) }
    fun update(block: (UiState) -> UiState) = _state.update(block)
}
