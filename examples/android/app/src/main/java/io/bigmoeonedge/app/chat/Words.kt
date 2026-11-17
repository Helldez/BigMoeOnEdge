package io.bigmoeonedge.app.chat

/**
 * The words of the conversation's status line, its welcome and the figures under an answer: the
 * web UI's (ui/src/lib/text/en.ts, keys `buddy.*`, `conversation.welcome*` and `figure.*`), kept in
 * one place so the three UIs say the same thing. Where the web says "this computer" the phone says
 * "this phone"; the welcome drops the folder clause, since a phone has no workspaces.
 */
object Words {
    const val UNREACHABLE = "The engine is not running."
    const val CONNECTING = "Connecting…"
    const val MEASURING = "Measuring this phone…"
    const val LOADING = "Loading the model…"
    const val LOAD_ERROR = "The model did not load. See Models."
    const val DISK = "Reading part of the model from the disk…"
    const val WRITING = "Writing…"
    const val THINKING = "Thinking…"
    const val NO_MODEL = "No model loaded"
    const val FAILED = "Something went wrong; see the note below"
    const val STOPPED = "Stopped"
    const val ANSWERED = "Answered"
    const val READY = "Ready"

    const val PRIVATE = "Private: everything stays on this phone."
    const val WELCOME = "What shall we do?"
    const val WELCOME_LINE = "Ask a question."
    /** Questions to try on an empty conversation, once a model can answer them. */
    val TRIES = listOf(
        "Plan three days in Lisbon in March, cheap and walkable.",
        "Explain compound interest with a simple example.",
        "Write a short, friendly email declining a meeting.",
    )
    const val COMPOSER_LOADING = "The model is loading; you can write as soon as it is ready."
    /** Beside a model's name wherever the engine's state is shown outside the conversation. */
    const val GENERATING = "Writing an answer"
    const val PRIVATE_HERE = "private, on this phone"

    const val SPEED = "Answer speed"
    const val SPEED_HINT = "Tokens written per second. A token is a piece of a word: about three quarters of a word in " +
        "English. The plan expected rate is what the plan predicted for this phone."
    const val CACHE = "Found in memory"
    const val CACHE_HINT = "Of the parts of the model each token needed, how many were already in memory instead of read " +
        "from the disk (the expert cache). More is faster."
    const val TIME = "Where each token's time went"
    const val TIME_HINT = "The time it took to write each token, split into what it was spent on. The parts add up to 100%."
    const val PROMPT = "Reading the message"
    const val PROMPT_HINT = "How fast the engine read what was new in this turn before writing. Earlier turns kept in the " +
        "context are not read again."
    const val CONTEXT = "Context used"
    const val CONTEXT_HINT = "How much of the model's memory for text (its context) this conversation takes. When it is " +
        "full, the conversation cannot go on: start a new one, or raise the context in Settings."
    const val DRAFTS = "Guesses kept"
    const val DRAFTS_HINT = "The share of tokens guessed ahead that the model confirmed. More kept means faster answers."
    const val STOPPED_HINT = "The answer was stopped before it finished."
    const val FIGURES_OPEN = "How it ran"
    const val TO_PLAN = "See the plan"
    const val TO_METRICS = "All figures"
}
