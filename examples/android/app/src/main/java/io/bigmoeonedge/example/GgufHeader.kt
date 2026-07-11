package io.bigmoeonedge.example

import java.io.BufferedInputStream
import java.io.EOFException
import java.io.File
import java.io.InputStream

/**
 * Minimal streaming reader for the gguf header — enough to tell a MoE model from a dense one
 * without loading tensor data. It walks the real structure (magic, version, KV metadata, then
 * the tensor-info block) instead of scanning a fixed prefix, so it works no matter how large
 * the KV section is (a big tokenizer can push the tensor names well past any fixed window).
 *
 * gguf layout: "GGUF" magic, u32 version, then counts and a key/value metadata block, then one
 * info record per tensor (name, dims, type, offset). We only need the tensor names: a MoE model
 * names its down projection `blk.<il>.ffn_down_exps`, absent from dense models.
 *
 * Spec: https://github.com/ggml-org/ggml/blob/master/docs/gguf.md
 */
object GgufHeader {
    private const val MOE_MARKER = "ffn_down_exps"

    // gguf value types (for skipping KV entries we don't read).
    private const val T_UINT8 = 0
    private const val T_INT8 = 1
    private const val T_UINT16 = 2
    private const val T_INT16 = 3
    private const val T_UINT32 = 4
    private const val T_INT32 = 5
    private const val T_FLOAT32 = 6
    private const val T_BOOL = 7
    private const val T_STRING = 8
    private const val T_ARRAY = 9
    private const val T_UINT64 = 10
    private const val T_INT64 = 11
    private const val T_FLOAT64 = 12

    // Guards against a corrupt/adversarial header: bound the work and reject absurd sizes
    // rather than allocating or looping unboundedly.
    private const val MAX_HEADER_BYTES = 64L * 1024 * 1024
    private const val MAX_STRING_LEN = 1L * 1024 * 1024
    private const val MAX_TENSORS = 1_000_000L

    /**
     * True if the gguf declares expert tensors (i.e. it is a MoE model). Reads only the header.
     * Falls back to a substring scan of the header region if the structured parse fails, so this
     * can only improve on the old probe, never miss a model the probe would have caught.
     */
    fun isMoe(f: File): Boolean =
        runCatching { f.inputStream().buffered().use { parseIsMoe(Counting(it)) } }
            .getOrElse { substringFallback(f) }

    private fun parseIsMoe(s: Counting): Boolean {
        val magic = ByteArray(4)
        s.readFully(magic)
        if (magic[0].toInt() != 'G'.code || magic[1].toInt() != 'G'.code ||
            magic[2].toInt() != 'U'.code || magic[3].toInt() != 'F'.code
        ) {
            throw IllegalArgumentException("not a gguf file")
        }
        val version = s.u32()
        // v1 used 32-bit counts and string lengths; v2+ use 64-bit. We support v2+ (every
        // current model); a v1 file throws and takes the substring fallback.
        if (version < 2) throw IllegalArgumentException("unsupported gguf version $version")

        val tensorCount = s.u64()
        val kvCount = s.u64()
        if (tensorCount < 0 || tensorCount > MAX_TENSORS || kvCount < 0) {
            throw IllegalArgumentException("implausible header counts")
        }

        // Skip the KV metadata block to reach the tensor-info records.
        for (i in 0 until kvCount) {
            skipString(s)                 // key
            skipValue(s, s.u32())         // value
            if (s.pos > MAX_HEADER_BYTES) throw IllegalArgumentException("header too large")
        }

        // Tensor-info records: we only need the names.
        for (i in 0 until tensorCount) {
            val name = readString(s)
            if (name.contains(MOE_MARKER)) return true
            val nDims = s.u32()
            if (nDims < 0 || nDims > 8) throw IllegalArgumentException("implausible tensor dims")
            s.skipFully(nDims * 8L)       // dims (u64 each)
            s.u32()                       // ggml type
            s.u64()                       // data offset
            if (s.pos > MAX_HEADER_BYTES) throw IllegalArgumentException("header too large")
        }
        return false
    }

    private fun skipValue(s: Counting, type: Int) {
        when (type) {
            T_UINT8, T_INT8, T_BOOL -> s.skipFully(1)
            T_UINT16, T_INT16 -> s.skipFully(2)
            T_UINT32, T_INT32, T_FLOAT32 -> s.skipFully(4)
            T_UINT64, T_INT64, T_FLOAT64 -> s.skipFully(8)
            T_STRING -> skipString(s)
            T_ARRAY -> {
                val elemType = s.u32()
                val count = s.u64()
                if (count < 0) throw IllegalArgumentException("bad array count")
                if (elemType == T_STRING) {
                    for (i in 0 until count) skipString(s)
                } else if (elemType == T_ARRAY) {
                    throw IllegalArgumentException("nested gguf arrays unsupported")
                } else {
                    s.skipFully(count * scalarSize(elemType))
                }
            }
            else -> throw IllegalArgumentException("unknown gguf value type $type")
        }
    }

    private fun scalarSize(type: Int): Long = when (type) {
        T_UINT8, T_INT8, T_BOOL -> 1
        T_UINT16, T_INT16 -> 2
        T_UINT32, T_INT32, T_FLOAT32 -> 4
        T_UINT64, T_INT64, T_FLOAT64 -> 8
        else -> throw IllegalArgumentException("non-scalar gguf type $type")
    }

    private fun readString(s: Counting): String {
        val len = s.u64()
        if (len < 0 || len > MAX_STRING_LEN) throw IllegalArgumentException("implausible string length $len")
        val bytes = ByteArray(len.toInt())
        s.readFully(bytes)
        return String(bytes, Charsets.UTF_8)
    }

    private fun skipString(s: Counting) {
        val len = s.u64()
        if (len < 0 || len > MAX_STRING_LEN) throw IllegalArgumentException("implausible string length $len")
        s.skipFully(len)
    }

    // Last-resort scan of the header region, matching the old behaviour so the structured parse
    // can only add detections. Reads up to 16 MiB — the tensor-info block starts right after the
    // KV metadata and is normally well within that.
    private fun substringFallback(f: File): Boolean = runCatching {
        val cap = minOf(f.length(), 16L * 1024 * 1024).toInt()
        val buf = ByteArray(cap)
        f.inputStream().use { s ->
            var off = 0
            while (off < cap) {
                val n = s.read(buf, off, cap - off)
                if (n <= 0) break
                off += n
            }
        }
        String(buf, Charsets.US_ASCII).contains(MOE_MARKER)
    }.getOrDefault(false)

    /** InputStream wrapper: little-endian primitive reads, exact skipping, and a byte counter. */
    private class Counting(private val src: InputStream) {
        var pos: Long = 0
            private set

        fun readFully(b: ByteArray) {
            var off = 0
            while (off < b.size) {
                val n = src.read(b, off, b.size - off)
                if (n < 0) throw EOFException()
                off += n
                pos += n
            }
        }

        fun skipFully(n: Long) {
            if (n < 0) throw IllegalArgumentException("negative skip")
            var left = n
            val scratch = ByteArray(8192)
            while (left > 0) {
                val step = src.skip(left)
                if (step > 0) {
                    left -= step
                    pos += step
                    continue
                }
                // skip() can return 0 legitimately; fall back to reading.
                val want = minOf(left, scratch.size.toLong()).toInt()
                val got = src.read(scratch, 0, want)
                if (got < 0) throw EOFException()
                left -= got
                pos += got
            }
        }

        fun u32(): Int {
            val b = ByteArray(4)
            readFully(b)
            return (b[0].toInt() and 0xff) or
                ((b[1].toInt() and 0xff) shl 8) or
                ((b[2].toInt() and 0xff) shl 16) or
                ((b[3].toInt() and 0xff) shl 24)
        }

        fun u64(): Long {
            val b = ByteArray(8)
            readFully(b)
            var v = 0L
            for (i in 0 until 8) v = v or ((b[i].toLong() and 0xff) shl (8 * i))
            return v
        }
    }
}
