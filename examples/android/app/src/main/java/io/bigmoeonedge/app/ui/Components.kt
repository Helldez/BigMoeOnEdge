package io.bigmoeonedge.app.ui

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import io.bigmoeonedge.app.data.Tone
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/** Shared UI atoms used by the screens. */

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun LabeledDropdown(label: String, options: List<String>, selected: Int, enabled: Boolean = true, onSelect: (Int) -> Unit) {
    var expanded by remember { mutableStateOf(false) }
    ExposedDropdownMenuBox(expanded = expanded, onExpandedChange = { if (enabled) expanded = it }) {
        OutlinedTextField(
            value = options.getOrElse(selected) { "" },
            onValueChange = {},
            readOnly = true,
            enabled = enabled,
            label = { Text(label) },
            trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = expanded) },
            modifier = Modifier.menuAnchor(MenuAnchorType.PrimaryNotEditable, enabled).fillMaxWidth(),
        )
        ExposedDropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
            options.forEachIndexed { i, opt ->
                DropdownMenuItem(text = { Text(opt) }, onClick = { onSelect(i); expanded = false })
            }
        }
    }
}

/** The ok and warn colours of the current appearance: what a state's colour is picked from. */
@Composable
fun okColor(): Color = tokens.ok

@Composable
fun warnColor(): Color = tokens.warn

/** The tint a state colour sits on in a badge or a banner, from the same token table. */
@Composable
fun tintFor(colour: Color): Color = with(tokens) {
    when (colour) {
        ok -> okTint
        warn -> warnTint
        err -> errTint
        accent, accentInk -> accentTint
        else -> colour.copy(alpha = 0.14f)
    }
}

/** A verdict's or a state's colour, from the tone a code picked; [Tone.NONE] is the muted text. */
@Composable
fun toneColor(t: Tone): Color = when (t) {
    Tone.OK -> okColor()
    Tone.WARN -> warnColor()
    Tone.ERR -> MaterialTheme.colorScheme.error
    Tone.NONE -> MaterialTheme.colorScheme.onSurfaceVariant
}

/** A small label on its colour's tint: a state, a source, a role. */
@Composable
fun Chip(text: String, colour: Color) {
    Surface(color = tintFor(colour), shape = MaterialTheme.shapes.extraSmall) {
        Text(
            text, fontSize = 11.sp, fontWeight = FontWeight.Medium, color = colour, maxLines = 1,
            modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
        )
    }
}

/**
 * A card: the surface on the page, a 1 dp line and a soft shadow. One place, so every box on every
 * screen has the same edge, corner and elevation.
 */
@Composable
fun BmoeCard(modifier: Modifier = Modifier, padding: Dp = 16.dp, colour: Color = tokens.surface, content: @Composable ColumnScope.() -> Unit) {
    Surface(
        color = colour,
        shape = Corner,
        shadowElevation = 1.dp,
        border = BorderStroke(1.dp, tokens.line),
        modifier = modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(padding), verticalArrangement = Arrangement.spacedBy(8.dp), content = content)
    }
}

/** A section title above a group of rows, as Material lists them. */
@Composable
fun SectionTitle(text: String, modifier: Modifier = Modifier) {
    Text(text, style = MaterialTheme.typography.titleSmall, color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = modifier)
}

/** Small explanatory line under a control. One place, so the styling cannot drift. */
@Composable
fun Hint(text: String) {
    Text(text, fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
}

@Composable
fun SwitchRow(label: String, description: String?, checked: Boolean, enabled: Boolean = true, onChange: (Boolean) -> Unit) {
    val dim = if (enabled) 1f else 0.38f // Material disabled-content alpha
    Row(
        Modifier.fillMaxWidth().padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(label, fontSize = 15.sp, color = MaterialTheme.colorScheme.onSurface.copy(alpha = dim))
            if (description != null) {
                Text(
                    description, fontSize = 12.sp,
                    color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = dim),
                )
            }
        }
        Switch(checked = checked, onCheckedChange = onChange, enabled = enabled)
    }
}
