package io.bigmoeonedge.example

/**
 * The MoE models this app offers as a one-tap download, so a first run needs no adb and no
 * hunting on Hugging Face. The list is deliberately short: these are the models the engine is
 * measured on (see docs/benchmarks.md), not a catalog of everything that happens to load.
 *
 * Any other MoE gguf still works — the URL field and the file picker below the catalog take
 * arbitrary models. Nothing here is baked into the engine: architectures are discovered at
 * runtime, this is only a convenience shortcut in the demo UI.
 */
object ModelCatalog {

    /**
     * One offered model.
     *
     * [url] null means "listed but not downloadable in-app" — [install] then carries the manual
     * recipe. [fileName] is authoritative: it is what lands on disk and what [statusOf] matches
     * against, so a redirect or a query string in the URL cannot rename the model.
     */
    data class Entry(
        val title: String,
        val quant: String,
        val fileName: String,
        val approxBytes: Long,
        val url: String?,
        /** Why you would pick this one. Kept short enough to sit on one line. */
        val blurb: String,
        /** Manual install steps, shown on demand. Non-null exactly when the entry is [Status.MANUAL_ONLY]. */
        val install: String? = null,
        /**
         * A model above Hugging Face's 50 GB per-file limit ships as several shard files; the engine
         * opens the FIRST shard and streams the whole set. For such an entry this lists every shard
         * in order, [fileName] is the first shard (the file handed to the engine, and the name the
         * whole set is keyed by in downloads and deletes), [approxBytes] is the total, and [url]
         * stays null — the shard URLs are here.
         */
        val shards: List<Shard> = emptyList(),
    )

    /** One file of a sharded entry. [bytes] is exact (from the repository), not approximate. */
    data class Shard(val fileName: String, val url: String, val bytes: Long)

    /**
     * Every on-disk name this entry can own: its own file plus any shards. The entry's [Entry.fileName]
     * is usually the first shard, but not always — gpt-oss keeps the legacy merged name so a file
     * merged by an earlier release still counts. Anything that asks "does this filename belong to a
     * catalog entry?" must ask THIS, not [Entry.fileName]: a shard answered no, and then showed up as
     * an imported model with a Delete that orphaned the rest of the set.
     */
    fun fileNamesOf(e: Entry): Set<String> = buildSet {
        add(e.fileName)
        e.shards.forEach { add(it.fileName) }
    }

    /** True if [name] is a file some catalog entry owns. */
    fun isCatalogFile(name: String): Boolean = entries.any { name in fileNamesOf(it) }

    enum class Status {
        /** Already on the device — nothing to do. */
        ON_DEVICE,

        /** A WorkManager download job for this file is in flight. */
        DOWNLOADING,

        /** Downloadable, not present yet. */
        AVAILABLE,

        /** Listed for reference; [Entry.install] carries the manual install recipe. */
        MANUAL_ONLY,
    }

    private const val GB = 1_000_000_000L

    val entries: List<Entry> = listOf(
        Entry(
            title = "Qwen3-30B-A3B",
            quant = "Q4_K_M",
            fileName = "Qwen3-30B-A3B-Q4_K_M.gguf",
            approxBytes = 18_556_686_912L,
            url = "https://huggingface.co/unsloth/Qwen3-30B-A3B-GGUF/resolve/main/" +
                "Qwen3-30B-A3B-Q4_K_M.gguf?download=true",
            blurb = "3B active of 30B. The published numbers use this one.",
        ),
        Entry(
            title = "Qwen3.6-35B-A3B",
            quant = "Q4_K_M",
            fileName = "Qwen_Qwen3.6-35B-A3B-Q4_K_M.gguf",
            approxBytes = 22_285_080_192L,
            url = "https://huggingface.co/bartowski/Qwen_Qwen3.6-35B-A3B-GGUF/resolve/main/" +
                "Qwen_Qwen3.6-35B-A3B-Q4_K_M.gguf?download=true",
            blurb = "3B active of 35B. Hybrid attention/SSM, comfortably past RAM.",
        ),
        Entry(
            title = "Gemma-4-26B-A4B-it",
            quant = "Q4_K_M",
            fileName = "google_gemma-4-26B-A4B-it-Q4_K_M.gguf",
            approxBytes = 17_035_038_112L,
            url = "https://huggingface.co/bartowski/google_gemma-4-26B-A4B-it-GGUF/resolve/main/" +
                "google_gemma-4-26B-A4B-it-Q4_K_M.gguf?download=true",
            blurb = "4B active of 26B. Smaller experts — the gentlest start.",
        ),
        Entry(
            title = "gpt-oss-120b",
            quant = "Q4_K_M",
            // The legacy name: earlier releases required merging the two shards on a PC, so a
            // merged file by this name on the device still counts as ON_DEVICE. New downloads
            // fetch the two shards as-is — the engine streams a split set natively.
            fileName = "gpt-oss-120b-Q4_K_M.gguf",
            approxBytes = 62_768_723_552L,
            url = null,
            blurb = "5B active of 117B. The >RAM case — two shards, downloaded as-is.",
            shards = listOf(
                Shard(
                    "gpt-oss-120b-Q4_K_M-00001-of-00002.gguf",
                    "https://huggingface.co/unsloth/gpt-oss-120b-GGUF/resolve/main/Q4_K_M/" +
                        "gpt-oss-120b-Q4_K_M-00001-of-00002.gguf?download=true",
                    49_630_904_192L,
                ),
                Shard(
                    "gpt-oss-120b-Q4_K_M-00002-of-00002.gguf",
                    "https://huggingface.co/unsloth/gpt-oss-120b-GGUF/resolve/main/Q4_K_M/" +
                        "gpt-oss-120b-Q4_K_M-00002-of-00002.gguf?download=true",
                    13_137_819_360L,
                ),
            ),
        ),
        Entry(
            title = "DeepSeek V4 Flash",
            quant = "UD-IQ2_M",
            fileName = "DeepSeek-V4-Flash-0731-UD-IQ2_M-00001-of-00003.gguf",
            approxBytes = 90_926_928_288L,
            url = null,
            blurb = "13B active of 284B. The biggest thing this app offers — needs ~95 GB free.",
            shards = listOf(
                Shard(
                    "DeepSeek-V4-Flash-0731-UD-IQ2_M-00001-of-00003.gguf",
                    "https://huggingface.co/unsloth/DeepSeek-V4-Flash-0731-GGUF/resolve/main/UD-IQ2_M/" +
                        "DeepSeek-V4-Flash-0731-UD-IQ2_M-00001-of-00003.gguf?download=true",
                    5_257_664L,
                ),
                Shard(
                    "DeepSeek-V4-Flash-0731-UD-IQ2_M-00002-of-00003.gguf",
                    "https://huggingface.co/unsloth/DeepSeek-V4-Flash-0731-GGUF/resolve/main/UD-IQ2_M/" +
                        "DeepSeek-V4-Flash-0731-UD-IQ2_M-00002-of-00003.gguf?download=true",
                    49_956_780_160L,
                ),
                Shard(
                    "DeepSeek-V4-Flash-0731-UD-IQ2_M-00003-of-00003.gguf",
                    "https://huggingface.co/unsloth/DeepSeek-V4-Flash-0731-GGUF/resolve/main/UD-IQ2_M/" +
                        "DeepSeek-V4-Flash-0731-UD-IQ2_M-00003-of-00003.gguf?download=true",
                    40_964_890_464L,
                ),
            ),
        ),
    )

    /**
     * How this app writes a size: decimal GB, one decimal, the same unit the model repositories
     * quote. Everything user-facing goes through here — quoting a model as 17.0 GB and then
     * refusing it for "15 GiB" reads as a bug even though both are true.
     */
    fun gbLabel(bytes: Long): String = String.format(java.util.Locale.US, "%.1f GB", bytes.toDouble() / GB)

    /** Size for the row label. */
    fun sizeLabel(e: Entry): String = "~" + gbLabel(e.approxBytes)

    /**
     * Status of one entry. [present] is the scanned model list (any scan dir counts, so a merged
     * gpt-oss pushed to /data/local/tmp/bmoe reads as ON_DEVICE), [downloading] the filenames of
     * in-flight downloads.
     */
    fun statusOf(e: Entry, present: Set<String>, downloading: Set<String>): Status = when {
        // A sharded entry is on-device when EVERY shard landed — or when a legacy pre-split file
        // by the entry name is present (gpt-oss merged on a PC by an earlier release). Any shard
        // still in flight makes the whole entry DOWNLOADING: the set is downloaded sequentially,
        // so exactly one transfers at a time but the entry is one unit of progress to the user.
        e.shards.isNotEmpty() -> when {
            e.fileName in present || e.shards.all { it.fileName in present } -> Status.ON_DEVICE
            e.shards.any { it.fileName in downloading } -> Status.DOWNLOADING
            else -> Status.AVAILABLE
        }
        e.fileName in present -> Status.ON_DEVICE
        e.fileName in downloading -> Status.DOWNLOADING
        e.url == null -> Status.MANUAL_ONLY
        else -> Status.AVAILABLE
    }
}
