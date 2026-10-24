package io.bigmoeonedge.app.ui

import androidx.compose.foundation.border
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.RectangleShape
import io.bigmoeonedge.app.data.Tone
import androidx.compose.ui.text.font.FontWeight
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

/** The palette's ok and warn colours for the current theme: what a state's colour is picked from. */
@Composable
fun okColor(): Color = if (isSystemInDarkTheme()) Palette.Ok else Palette.LightOk

@Composable
fun warnColor(): Color = if (isSystemInDarkTheme()) Palette.Warn else Palette.LightWarn

/** A verdict's or a state's colour, from the tone a code picked; [Tone.NONE] is the muted text. */
@Composable
fun toneColor(t: Tone): Color = when (t) {
    Tone.OK -> okColor()
    Tone.WARN -> warnColor()
    Tone.ERR -> MaterialTheme.colorScheme.error
    Tone.NONE -> MaterialTheme.colorScheme.onSurfaceVariant
}

/** A small square label with a coloured edge: a state, a source, a role. */
@Composable
fun Chip(text: String, colour: Color) {
    Surface(color = colour.copy(alpha = 0.18f), shape = RectangleShape, modifier = Modifier.border(2.dp, colour)) {
        Text(text, fontSize = 11.sp, color = colour, maxLines = 1, modifier = Modifier.padding(horizontal = 6.dp, vertical = 1.dp))
    }
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
