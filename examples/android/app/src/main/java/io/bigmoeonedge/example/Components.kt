package io.bigmoeonedge.example

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.KeyboardArrowUp
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/** Shared UI atoms used by the main and settings screens. */

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
            modifier = Modifier.menuAnchor().fillMaxWidth(),
        )
        ExposedDropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
            options.forEachIndexed { i, opt ->
                DropdownMenuItem(text = { Text(opt) }, onClick = { onSelect(i); expanded = false })
            }
        }
    }
}

/** Dropdown backed by an int array of choices; reports the chosen value. */
@Composable
fun IntSetting(
    label: String,
    choices: IntArray,
    value: Int,
    format: (Int) -> String = { it.toString() },
    enabled: Boolean = true,
    onSelect: (Int) -> Unit,
) {
    val idx = choices.indexOf(value).coerceAtLeast(0)
    LabeledDropdown(label, choices.map(format), idx, enabled) { onSelect(choices[it]) }
}

/**
 * A collapsed group for settings that are not part of the recommended configuration.
 *
 * Closed by default, so the screen a user scrolls shows the settings that are known to work and
 * nothing else. What is inside is not hidden, only folded: these are the levers this engine exists
 * to test, and a demo app that concealed them would be lying about what it is for.
 *
 * The caveat is stated once here rather than repeated in every description, which is what let the
 * word "experimental" drift into some blurbs and out of others.
 */
@Composable
fun ExperimentalGroup(content: @Composable ColumnScope.() -> Unit) {
    var open by rememberSaveable { mutableStateOf(false) }
    Column(Modifier.fillMaxWidth().padding(top = 4.dp)) {
        Row(
            Modifier.fillMaxWidth().clickable { open = !open }.padding(vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(
                if (open) Icons.Filled.KeyboardArrowUp else Icons.Filled.KeyboardArrowDown,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Spacer(Modifier.width(4.dp))
            Text(
                "Experimental",
                fontSize = 14.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        if (open) {
            Text(
                "Measured on one device, or not measured yet. These can slow things down or change " +
                    "the reply. The settings above are the recommended configuration.",
                fontSize = 12.sp,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(bottom = 8.dp),
            )
            Column(verticalArrangement = Arrangement.spacedBy(8.dp), content = content)
        }
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
