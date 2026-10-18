package io.bigmoeonedge.app.ui

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.selection.selectable
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.RectangleShape
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import io.bigmoeonedge.app.data.HfFacts
import io.bigmoeonedge.app.data.HfModel
import io.bigmoeonedge.app.data.gbLabel
import io.bigmoeonedge.app.engine.Engine
import kotlinx.coroutines.launch

/**
 * A model from a Hugging Face repository, in the three steps the server does (docs/server-api.md,
 * "Models"): list what the repository holds, read one model's header against this phone, add it to
 * the catalog. [onAdded] then starts the download the way every catalog entry is downloaded here.
 * The facts card shows what the server answered; the app reads no gguf.
 */
@Composable
fun HuggingFaceSection(onAdded: suspend (id: String) -> Unit) {
    val scope = rememberCoroutineScope()
    var repo by rememberSaveable { mutableStateOf("") }
    var token by rememberSaveable { mutableStateOf("") }
    var askToken by rememberSaveable { mutableStateOf(false) }
    var listed by remember { mutableStateOf("") }
    var models by remember { mutableStateOf<List<HfModel>?>(null) }
    var selected by remember { mutableStateOf("") }
    var facts by remember { mutableStateOf<HfFacts?>(null) }
    var busy by remember { mutableStateOf("") }
    var error by remember { mutableStateOf("") }
    var added by remember { mutableStateOf("") }

    val chosen = models?.firstOrNull { it.name == selected }

    fun run(what: String, block: suspend () -> Unit) {
        scope.launch {
            busy = what
            error = ""
            try {
                block()
            } catch (e: Exception) {
                error = e.message ?: e.toString()
            } finally {
                busy = ""
            }
        }
    }

    fun list() = run("list") {
        facts = null
        added = ""
        val r = Engine.hfModels(repo, token).getOrThrow()
        models = r.models
        listed = r.repo
        // A URL that named a file preselects it; a repository with one model needs no choice.
        selected = when {
            r.file.isNotEmpty() && r.models.any { it.name == r.file } -> r.file
            r.models.size == 1 -> r.models[0].name
            else -> ""
        }
    }

    fun inspect() = run("inspect") { facts = Engine.hfInspect(listed, selected, token).getOrThrow() }

    fun add() = run("add") {
        val id = Engine.hfAdd(listed, selected, token).getOrThrow()
        added = id
        onAdded(id)
    }

    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(
            "Any gguf on Hugging Face: the server lists the repository, reads a model's header before any download, and says what it is and whether it fits here.",
            fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        OutlinedTextField(
            value = repo, onValueChange = { repo = it },
            label = { Text("Repository (owner/name, or a huggingface.co link)") },
            singleLine = true, modifier = Modifier.fillMaxWidth(),
        )
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Button(
                shape = RectangleShape, onClick = { list() },
                enabled = repo.isNotBlank() && busy.isEmpty(), modifier = Modifier.weight(1f),
            ) { Text(if (busy == "list") "Listing…" else "List") }
            OutlinedButton(shape = RectangleShape, onClick = { askToken = !askToken }, modifier = Modifier.weight(1f)) {
                Text(if (askToken) "Hide token" else "Token")
            }
        }
        if (askToken) {
            OutlinedTextField(
                value = token, onValueChange = { token = it },
                label = { Text("Access token, for a gated or private repository") },
                supportingText = { Text("Used for these requests and kept nowhere.", fontSize = 11.sp) },
                singleLine = true, visualTransformation = PasswordVisualTransformation(), modifier = Modifier.fillMaxWidth(),
            )
        }
        if (error.isNotEmpty()) Text(error, fontSize = 12.sp, color = MaterialTheme.colorScheme.error)

        models?.let { list ->
            if (list.isEmpty()) {
                Text("No gguf files in $listed.", fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            } else {
                list.forEach { m ->
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        modifier = Modifier
                            .fillMaxWidth()
                            .selectable(selected = m.name == selected, onClick = { selected = m.name; facts = null; added = "" }),
                    ) {
                        RadioButton(selected = m.name == selected, onClick = null)
                        Column(Modifier.weight(1f)) {
                            Text(m.name, fontSize = 13.sp, maxLines = 2, overflow = TextOverflow.Ellipsis)
                            Text(
                                listOfNotNull(
                                    m.quant.takeIf { it.isNotEmpty() },
                                    "~${gbLabel(m.bytes)}",
                                    if (m.count > 1) "${m.count} shards" else null,
                                    if (!m.complete) "incomplete" else null,
                                ).joinToString(" · "),
                                fontSize = 11.sp, color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                    }
                }
                Button(
                    shape = RectangleShape, onClick = { inspect() },
                    enabled = chosen != null && busy.isEmpty(), modifier = Modifier.fillMaxWidth(),
                ) { Text(if (busy == "inspect") "Reading the header…" else "Check") }
            }
        }

        facts?.let { f ->
            if (chosen == null) return@let
            HorizontalDivider()
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(f.name, fontSize = 13.sp, fontWeight = FontWeight.Medium, maxLines = 2, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f))
                Text(
                    f.mode, fontSize = 12.sp,
                    color = when (f.mode) {
                        "streams" -> MaterialTheme.colorScheme.primary
                        "not readable" -> MaterialTheme.colorScheme.error
                        else -> MaterialTheme.colorScheme.onSurfaceVariant
                    },
                )
            }
            if (f.readable) {
                val lines = buildList {
                    add("Architecture: ${f.arch.ifEmpty { "?" }}")
                    add(if (f.isMoe) "Experts: ${f.nExpert}, ${f.nExpertUsed} per token" else "Experts: none (dense)")
                    if (f.streamable) {
                        add("Layers: ${f.nLayer} (${f.nMoeLayer} with experts)")
                        add("Resident: ~${gbLabel(f.denseBytes)}")
                        add("Experts on flash: ~${gbLabel(f.expertBytes)}, ~${f.tokenCycleBytes shr 20} MiB a token")
                    }
                    add("On disk: ~${gbLabel(f.bytes)}" + if (f.files > 1) " in ${f.files} files" else "")
                }
                lines.forEach { Text(it, fontSize = 12.sp) }
            }
            Text(f.memory, fontSize = 12.sp, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Text(f.disk, fontSize = 12.sp, color = if (f.downloadable) MaterialTheme.colorScheme.onSurfaceVariant else MaterialTheme.colorScheme.error)
            if (added.isNotEmpty()) {
                Text("Added to the catalog below.", fontSize = 12.sp, color = MaterialTheme.colorScheme.primary)
            } else {
                Button(
                    shape = RectangleShape, onClick = { add() },
                    enabled = f.downloadable && chosen.complete && busy.isEmpty(), modifier = Modifier.fillMaxWidth(),
                ) { Text(if (busy == "add") "Adding…" else "Add to the catalog and download") }
            }
        }
    }
}
