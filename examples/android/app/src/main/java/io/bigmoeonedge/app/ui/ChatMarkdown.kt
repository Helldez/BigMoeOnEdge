package io.bigmoeonedge.app.ui

import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextLinkStyles
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.mikepenz.markdown.compose.components.markdownComponents
import com.mikepenz.markdown.compose.elements.LocalTableRowIndex
import com.mikepenz.markdown.compose.elements.MarkdownDivider
import com.mikepenz.markdown.compose.elements.MarkdownTable
import com.mikepenz.markdown.compose.elements.MarkdownTableHeader
import com.mikepenz.markdown.compose.elements.MarkdownTableRow
import com.mikepenz.markdown.m3.Markdown
import com.mikepenz.markdown.m3.markdownColor
import com.mikepenz.markdown.m3.markdownTypography
import com.mikepenz.markdown.model.markdownDimens
import com.mikepenz.markdown.model.rememberMarkdownState
import org.intellij.markdown.ast.ASTNode

/**
 * A model's answer as GitHub-flavoured Markdown (tables included), drawn by the renderer library in
 * the app's tokens and fonts. Nothing in an answer reaches the network by being shown: no image
 * loader is configured, so an image renders as nothing, and raw HTML is never interpreted. A link
 * opens only when tapped.
 */
@Composable
fun ChatMarkdown(text: String, modifier: Modifier = Modifier, fontSize: TextUnit = 15.sp) {
    val t = tokens
    val body = MaterialTheme.typography.bodyLarge.copy(fontSize = fontSize, lineHeight = fontSize * 1.4f, color = t.text)
    fun heading(scale: Float) = body.copy(fontSize = fontSize * scale, lineHeight = fontSize * scale * 1.3f, fontWeight = FontWeight.SemiBold)
    val code = CodeStyle.copy(fontSize = fontSize * 0.85f, lineHeight = fontSize * 0.85f * 1.4f, color = t.text)

    // An answer grows token by token: keeping the last parse on screen while the next one runs
    // stops the message from blanking between tokens.
    val state = rememberMarkdownState(text, retainState = true)
    Markdown(
        markdownState = state,
        modifier = modifier.fillMaxWidth(),
        colors = markdownColor(
            text = t.text,
            codeBackground = t.surface2,
            inlineCodeBackground = t.surface2,
            dividerColor = t.line,
            tableBackground = Color.Transparent,
        ),
        typography = markdownTypography(
            h1 = heading(1.5f),
            h2 = heading(1.3f),
            h3 = heading(1.15f),
            h4 = heading(1f),
            h5 = heading(1f),
            h6 = heading(1f),
            text = body,
            paragraph = body,
            ordered = body,
            bullet = body,
            list = body,
            table = body,
            quote = body.copy(fontStyle = FontStyle.Italic, color = t.muted),
            code = code,
            inlineCode = body.copy(fontFamily = CodeFont, fontSize = fontSize * 0.9f),
            textLink = TextLinkStyles(SpanStyle(color = t.accentInk, textDecoration = TextDecoration.Underline)),
        ),
        dimens = markdownDimens(
            codeBackgroundCornerSize = 6.dp,
            tableCellWidth = TABLE_COLUMN_WIDTH,
            tableCellPadding = 8.dp,
            tableCornerSize = 6.dp,
        ),
        components = markdownComponents(table = { model -> BorderedTable(model.content, model.node, model.typography.table) }),
    )
}

/**
 * The width every table column gets. The library gives each column the same width and scrolls the
 * table sideways once the columns no longer fit, so this is what keeps a wide table readable on a
 * phone instead of squeezing it into slivers.
 */
private val TABLE_COLUMN_WIDTH = 140.dp

/**
 * The library's table, with a border in the line token around it and between rows, and cells that
 * wrap: by default a cell keeps one line and ellipsizes, which hides what a model put in a table.
 */
@Composable
private fun BorderedTable(content: String, node: ASTNode, style: TextStyle) {
    val line = tokens.line
    val shape = RoundedCornerShape(6.dp)
    Box(Modifier.padding(vertical = 4.dp).clip(shape).border(1.dp, line, shape)) {
        MarkdownTable(
            content = content,
            node = node,
            style = style,
            headerBlock = { c, header, width, s ->
                MarkdownTableHeader(c, header, width, s, maxLines = Int.MAX_VALUE)
            },
            rowBlock = { c, row, width, s ->
                // The header's own separator already rules off the first row.
                if (LocalTableRowIndex.current > 1) MarkdownDivider()
                MarkdownTableRow(c, row, width, s, maxLines = Int.MAX_VALUE)
            },
        )
    }
}
