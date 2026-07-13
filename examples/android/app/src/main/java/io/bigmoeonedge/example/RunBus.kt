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

/** Immutable snapshot of the session + current generation, observed by the Compose UI. */
data class UiState(
    val state: EngineState = EngineState.IDLE,
    val telemetry: Telemetry = Telemetry(),
    val answer: String = "",
    val summary: String = "",
    val error: String? = null,
    val ioMode: String? = null,     // effective read mode reported by the engine (direct / buffered)
    val sessionSig: String? = null, // signature of the loaded session (AppSettings.sessionSignature)
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
        it.copy(telemetry = Telemetry(), answer = "", summary = "", error = null)
    }

    fun setState(s: EngineState) = _state.update { it.copy(state = s) }
    fun update(block: (UiState) -> UiState) = _state.update(block)
}
