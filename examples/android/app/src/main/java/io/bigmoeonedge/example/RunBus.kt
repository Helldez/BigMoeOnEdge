package io.bigmoeonedge.example

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update

/** Immutable snapshot of a run, observed by the Compose UI. */
data class UiState(
    val running: Boolean = false,
    val loading: Boolean = false, // model started, first token not yet emitted
    val telemetry: Telemetry = Telemetry(),
    val answer: String = "",
    val summary: String = "",
    val error: String? = null,
    val ioMode: String? = null, // effective read mode reported by the engine (direct / buffered)
)

/**
 * Single source of truth shared between the RunService (writer) and the UI (reader).
 * The service pushes updates as CLI output arrives; the UI collects the StateFlow. One
 * generation runs at a time, so a single flow is enough.
 */
object RunBus {
    private val _state = MutableStateFlow(UiState())
    val state: StateFlow<UiState> = _state.asStateFlow()

    fun reset() = _state.update { UiState(running = it.running) }
    fun setRunning(running: Boolean) =
        _state.update { it.copy(running = running, loading = if (running) it.loading else false) }
    fun setLoading(loading: Boolean) = _state.update { it.copy(loading = loading) }
    fun update(block: (UiState) -> UiState) = _state.update(block)
}
