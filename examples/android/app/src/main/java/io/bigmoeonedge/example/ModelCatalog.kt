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
        /** Manual install steps, shown on demand. Non-null exactly when [url] is null. */
        val install: String? = null,
    )

    enum class Status {
        /** Already on the device — nothing to do. */
        ON_DEVICE,

        /** A DownloadManager job for this file is in flight. */
        DOWNLOADING,

        /** Downloadable, not present yet. */
        AVAILABLE,

        /** Listed for reference; [Entry.notes] carries the manual install recipe. */
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
            fileName = "gpt-oss-120b-Q4_K_M.gguf",
            approxBytes = 62_768_723_552L,
            // Not downloadable in-app: Hugging Face ships this quant as two shards (the 50 GB
            // per-file limit), and expert streaming reads tensors by byte offset from one file.
            // Merging on the phone would need ~120 GB of free space, so it is a PC step.
            url = null,
            blurb = "5B active of 117B. The >RAM case — merge it on a PC.",
            install = "Hugging Face ships this quant as two shards, and expert streaming needs a\n" +
                "single file. Merging on the phone would need ~120 GB free, so merge on a PC:\n" +
                "\n" +
                "1. Download both shards from\n" +
                "   huggingface.co/unsloth/gpt-oss-120b-GGUF (folder Q4_K_M/)\n" +
                "2. llama-gguf-split --merge \\\n" +
                "     gpt-oss-120b-Q4_K_M-00001-of-00002.gguf \\\n" +
                "     gpt-oss-120b-Q4_K_M.gguf\n" +
                "3. Move the merged file to the phone" +
                if (BuildConfig.SHARED_STORAGE) {
                    ":\n   adb push gpt-oss-120b-Q4_K_M.gguf /data/local/tmp/bmoe/"
                } else {
                    ", then pick it with\n   \"Other model\" below."
                },
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
        e.fileName in present -> Status.ON_DEVICE
        e.fileName in downloading -> Status.DOWNLOADING
        e.url == null -> Status.MANUAL_ONLY
        else -> Status.AVAILABLE
    }
}
