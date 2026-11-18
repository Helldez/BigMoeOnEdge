package io.bigmoeonedge.app.chat

/**
 * The status line at the top of the conversation: a few plain words on what the engine and the open
 * conversation are doing, so a person never needs a figure to know whether it is thinking, reading
 * part of the model from the disk, done, or stuck. The same rules as the web UI's chip
 * (ui/src/components/ChipBuddy.svelte); the words are [Words]'.
 */
data class ConversationStatus(val text: String, val mood: Mood) {
    /** How the logo beside the words looks: still, breathing while working, warned, or dimmed. */
    enum class Mood { IDLE, BUSY, ALERT, SLEEP }

    /** What the line is worked out from: the engine's state and the open conversation's, nothing else. */
    data class Facts(
        val serverUp: Boolean,
        /** `GET /api/info` has answered at least once. */
        val infoKnown: Boolean,
        /** The engine's state: empty, planning, loading, ready or error. */
        val engineState: String,
        /** An answer is being sent or written in the open conversation. */
        val running: Boolean = false,
        /** The newest item of the conversation is an answer with words in it. */
        val writing: Boolean = false,
        /** The newest item of the conversation is an answer. */
        val lastIsAnswer: Boolean = false,
        /** The open conversation ended in an error. */
        val failed: Boolean = false,
        /** The last finished turn was stopped. */
        val cancelled: Boolean = false,
        /** The live token's wait for the disk and its whole time, in ms (0 when no token is live). */
        val flashWaitMs: Double = 0.0,
        val wallMs: Double = 0.0,
    )

    companion object {
        /** The share of a token's time spent waiting for the disk above which the line says so. */
        const val DISK_SAYS = 0.3

        /**
         * The line for [f]. [greet]: the welcome of an empty conversation, which says, when all is
         * ready, that everything stays on the phone instead of a bare "Ready".
         */
        fun of(f: Facts, greet: Boolean = false): ConversationStatus = when {
            !f.serverUp -> ConversationStatus(Words.UNREACHABLE, Mood.SLEEP)
            !f.infoKnown -> ConversationStatus(Words.CONNECTING, Mood.BUSY)
            f.engineState == "planning" -> ConversationStatus(Words.MEASURING, Mood.BUSY)
            f.engineState == "loading" -> ConversationStatus(Words.LOADING, Mood.BUSY)
            f.engineState == "error" -> ConversationStatus(Words.LOAD_ERROR, Mood.ALERT)
            f.running -> when {
                f.wallMs > 0 && f.flashWaitMs / f.wallMs > DISK_SAYS -> ConversationStatus(Words.DISK, Mood.BUSY)
                f.writing -> ConversationStatus(Words.WRITING, Mood.BUSY)
                else -> ConversationStatus(Words.THINKING, Mood.BUSY)
            }
            f.engineState == "empty" -> ConversationStatus(Words.NO_MODEL, Mood.SLEEP)
            f.failed -> ConversationStatus(Words.FAILED, Mood.ALERT)
            f.cancelled && f.lastIsAnswer -> ConversationStatus(Words.STOPPED, Mood.IDLE)
            f.lastIsAnswer -> ConversationStatus(Words.ANSWERED, Mood.IDLE)
            else -> ConversationStatus(if (greet) Words.PRIVATE else Words.READY, Mood.IDLE)
        }
    }
}
