package io.bigmoeonedge.example

import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/**
 * A small Markdown renderer for chat answers — headings, lists, quotes, rules, fenced code and the
 * usual inline emphasis. Deliberately hand-rolled instead of pulling a rendering library into the
 * demo APK: the subset a chat model actually emits is tiny, and a parser we own can render the
 * half-finished markup of a streaming answer (an unclosed fence, a dangling `**`) as plain text
 * rather than swallowing it.
 */
@Composable
fun MarkdownText(text: String, modifier: Modifier = Modifier, fontSize: TextUnit = 15.sp) {
    val codeBg = MaterialTheme.colorScheme.surfaceVariant
    val blocks = remember(text) { parseBlocks(text) }
    Column(modifier, verticalArrangement = Arrangement.spacedBy(6.dp)) {
        blocks.forEach { block ->
            when (block) {
                is Block.Heading -> Text(
                    inline(block.text, codeBg),
                    fontSize = headingSize(block.level, fontSize),
                    fontWeight = FontWeight.Bold,
                )

                is Block.Para -> Text(inline(block.text, codeBg), fontSize = fontSize)

                is Block.Item -> Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    Text(
                        block.marker,
                        Modifier.padding(start = (block.indent * 12).dp).width(if (block.ordered) 22.dp else 12.dp),
                        fontSize = fontSize,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Text(inline(block.text, codeBg), fontSize = fontSize, modifier = Modifier.weight(1f))
                }

                is Block.Quote -> Row(
                    Modifier.height(IntrinsicSize.Min),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    Box(
                        Modifier.width(3.dp).fillMaxHeight()
                            .background(MaterialTheme.colorScheme.primary, RoundedCornerShape(2.dp))
                    )
                    Text(
                        inline(block.text, codeBg),
                        fontSize = fontSize,
                        fontStyle = FontStyle.Italic,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }

                is Block.Code -> Surface(color = codeBg, shape = RoundedCornerShape(6.dp)) {
                    Column(Modifier.fillMaxWidth().padding(10.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        if (block.lang.isNotEmpty()) {
                            Text(
                                block.lang,
                                fontSize = 11.sp,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                        Text(
                            block.code,
                            Modifier.horizontalScroll(rememberScrollState()),
                            fontFamily = FontFamily.Monospace,
                            fontSize = fontSize * 0.85f,
                        )
                    }
                }

                Block.Rule -> HorizontalDivider()
            }
        }
    }
}

private fun headingSize(level: Int, base: TextUnit): TextUnit = when (level) {
    1 -> base * 1.5f
    2 -> base * 1.3f
    3 -> base * 1.15f
    else -> base
}

private sealed interface Block {
    data class Heading(val level: Int, val text: String) : Block
    data class Para(val text: String) : Block
    data class Item(val marker: String, val text: String, val indent: Int, val ordered: Boolean) : Block
    data class Quote(val text: String) : Block
    data class Code(val code: String, val lang: String) : Block
    data object Rule : Block
}

private val HEADING = Regex("""^(#{1,6})\s+(.*)$""")
private val BULLET = Regex("""^([-*+])\s+(.*)$""")
private val ORDERED = Regex("""^(\d{1,3})[.)]\s+(.*)$""")
private val RULE = Regex("""^(-{3,}|\*{3,}|_{3,})$""")
private val QUOTE = Regex("""^>\s?(.*)$""")
private val LINK = Regex("""^\[([^\]\n]*)\]\(([^)\s]*)[^)]*\)""")

private fun parseBlocks(src: String): List<Block> {
    val out = ArrayList<Block>()
    val para = StringBuilder()

    fun flushPara() {
        if (para.isNotEmpty()) {
            out += Block.Para(para.toString())
            para.setLength(0)
        }
    }

    val lines = src.split("\n")
    var i = 0
    while (i < lines.size) {
        val raw = lines[i]
        val line = raw.trim()
        val indent = (raw.length - raw.trimStart().length) / 2
        when {
            line.startsWith("```") -> {
                flushPara()
                val lang = line.removePrefix("```").trim()
                val code = StringBuilder()
                i++
                while (i < lines.size && !lines[i].trim().startsWith("```")) {
                    code.append(lines[i]).append('\n')
                    i++
                }
                i++ // The closing fence, which a still-streaming answer has not emitted yet.
                out += Block.Code(code.toString().trimEnd('\n'), lang)
            }

            line.isEmpty() -> {
                flushPara()
                i++
            }

            RULE.matches(line) -> {
                flushPara()
                out += Block.Rule
                i++
            }

            else -> {
                val heading = HEADING.matchEntire(line)
                val bullet = BULLET.matchEntire(line)
                val ordered = ORDERED.matchEntire(line)
                val quote = QUOTE.matchEntire(line)
                when {
                    heading != null -> {
                        flushPara()
                        out += Block.Heading(heading.groupValues[1].length, heading.groupValues[2])
                    }
                    bullet != null -> {
                        flushPara()
                        out += Block.Item("•", bullet.groupValues[2], indent, ordered = false)
                    }
                    ordered != null -> {
                        flushPara()
                        out += Block.Item(ordered.groupValues[1] + ".", ordered.groupValues[2], indent, ordered = true)
                    }
                    quote != null -> {
                        flushPara()
                        out += Block.Quote(quote.groupValues[1])
                    }
                    // Soft line breaks inside a paragraph are kept: chat models use them as real
                    // breaks far more often than as reflowable wrapping.
                    else -> para.append(if (para.isEmpty()) "" else "\n").append(line)
                }
                i++
            }
        }
    }
    flushPara()
    return out
}

private fun inline(src: String, codeBg: Color): AnnotatedString = buildAnnotatedString { appendInline(src, codeBg) }

/**
 * Inline emphasis. Every marker needs a closing partner: an unmatched one is emitted verbatim, so a
 * streaming answer shows its literal `**` for a moment instead of the rest of the text flipping
 * bold. Single `_` is not an italic marker on purpose — it would mangle identifiers like use_mmap.
 */
private fun AnnotatedString.Builder.appendInline(src: String, codeBg: Color) {
    var i = 0
    while (i < src.length) {
        val c = src[i]
        val pair = when {
            src.startsWith("**", i) -> "**"
            src.startsWith("__", i) -> "__"
            src.startsWith("~~", i) -> "~~"
            else -> null
        }
        when {
            c == '`' -> {
                val end = src.indexOf('`', i + 1)
                if (end < 0) {
                    append(c); i++
                } else {
                    withStyle(SpanStyle(fontFamily = FontFamily.Monospace, background = codeBg)) {
                        append(src.substring(i + 1, end))
                    }
                    i = end + 1
                }
            }

            pair != null -> {
                val end = src.indexOf(pair, i + 2)
                if (end < 0) {
                    append(pair); i += 2
                } else {
                    val style = if (pair == "~~") {
                        SpanStyle(textDecoration = TextDecoration.LineThrough)
                    } else {
                        SpanStyle(fontWeight = FontWeight.Bold)
                    }
                    withStyle(style) { appendInline(src.substring(i + 2, end), codeBg) }
                    i = end + pair.length
                }
            }

            // A lone '*' is italic only when it opens a run: "2 * 3" and a stray bullet stay literal.
            c == '*' && i + 1 < src.length && !src[i + 1].isWhitespace() -> {
                val end = src.indexOf('*', i + 1)
                if (end < 0) {
                    append(c); i++
                } else {
                    withStyle(SpanStyle(fontStyle = FontStyle.Italic)) {
                        appendInline(src.substring(i + 1, end), codeBg)
                    }
                    i = end + 1
                }
            }

            c == '[' -> {
                val link = LINK.find(src, i)?.takeIf { it.range.first == i }
                if (link == null) {
                    append(c); i++
                } else {
                    // The target is shown, not opened: the demo has no browser intent and a model
                    // can hallucinate a URL, so the text carries the link and the user decides.
                    withStyle(SpanStyle(textDecoration = TextDecoration.Underline)) {
                        appendInline(link.groupValues[1], codeBg)
                    }
                    i = link.range.last + 1
                }
            }

            else -> {
                append(c); i++
            }
        }
    }
}
