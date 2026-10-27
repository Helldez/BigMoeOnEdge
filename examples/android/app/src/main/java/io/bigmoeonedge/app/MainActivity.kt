package io.bigmoeonedge.app

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.List
import androidx.compose.material.icons.filled.DateRange
import androidx.compose.material.icons.filled.Email
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.compose.runtime.remember
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.core.content.ContextCompat
import android.content.Intent
import androidx.activity.compose.BackHandler
import io.bigmoeonedge.app.agent.Agent
import io.bigmoeonedge.app.agent.AgentNotifier
import io.bigmoeonedge.app.engine.Engine
import io.bigmoeonedge.app.ui.AgentNav
import io.bigmoeonedge.app.ui.AgentPage
import io.bigmoeonedge.app.ui.ProvidersScreen
import io.bigmoeonedge.app.ui.SkillsScreen
import io.bigmoeonedge.app.ui.TasksScreen
import io.bigmoeonedge.app.ui.WorkspacesScreen
import io.bigmoeonedge.app.ui.BmoeTheme
import androidx.compose.foundation.layout.Column
import io.bigmoeonedge.app.ui.ChatScreen
import io.bigmoeonedge.app.ui.EngineStatusLine
import io.bigmoeonedge.app.ui.MetricsScreen
import io.bigmoeonedge.app.ui.ModelsScreen
import io.bigmoeonedge.app.ui.PlanScreen
import io.bigmoeonedge.app.ui.SettingsScreen

/**
 * The app: five screens over the one engine, the same five every UI of this project has (chat,
 * models, plan, settings, metrics), each a client of bmoe-server on loopback, and the agent's own
 * pages (workspaces, skills, tasks, providers) opened over them. The server starts with the app and
 * stays up in a foreground service; the model it holds is loaded from Models.
 */
class MainActivity : ComponentActivity() {

    private val requestNotif = registerForActivityResult(ActivityResultContracts.RequestPermission()) { }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) {
            requestNotif.launch(Manifest.permission.POST_NOTIFICATIONS)
        }
        Agent.init(this)
        Engine.start(this)
        openFrom(intent)
        setContent {
            BmoeTheme {
                Surface(color = MaterialTheme.colorScheme.background) { Root() }
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        openFrom(intent)
    }

    // The agent notifies only while no screen of the app is on the display.
    override fun onStart() {
        super.onStart()
        Agent.foreground = true
    }

    override fun onStop() {
        Agent.foreground = false
        super.onStop()
    }

    /** A notification's tap names a conversation: the chat opens on it. */
    private fun openFrom(intent: Intent?) {
        intent?.getStringExtra(AgentNotifier.EXTRA_CONVERSATION)?.let { Agent.requestOpen(it) }
    }
}

private enum class Screen(val label: String, val icon: ImageVector) {
    CHAT("Chat", Icons.Filled.Email),
    MODELS("Models", Icons.AutoMirrored.Filled.List),
    PLAN("Plan", Icons.Filled.Info),
    SETTINGS("Settings", Icons.Filled.Settings),
    METRICS("Metrics", Icons.Filled.DateRange),
}

@Composable
private fun Root() {
    var screen by rememberSaveable { mutableStateOf(Screen.CHAT) }
    // The setting a link points at (the Plan screen, the context warning): Settings lands on it.
    var settingTarget by rememberSaveable { mutableStateOf("") }
    // An agent page open over the five (from the chat's drawer or Settings); Back closes it.
    var page by rememberSaveable { mutableStateOf<AgentPage?>(null) }
    val openSetting = { key: String -> settingTarget = key; page = null; screen = Screen.SETTINGS }
    val openRequest by Agent.openRequest.collectAsStateWithLifecycle()
    LaunchedEffect(openRequest) {
        if (Agent.consumeOpenRequest() != null) {
            page = null
            screen = Screen.CHAT
        }
    }
    BackHandler(enabled = page != null) { page = null }
    // With nothing on the phone there is nothing to chat with: the first listing of a launch, if it is
    // empty, opens Models instead. Once: a person who then goes to Chat stays there.
    var firstListingSeen by rememberSaveable { mutableStateOf(false) }
    val models by remember { Engine.state.map { it.models }.distinctUntilChanged() }.collectAsStateWithLifecycle(null)
    LaunchedEffect(models) {
        val m = models ?: return@LaunchedEffect
        if (firstListingSeen) return@LaunchedEffect
        firstListingSeen = true
        if (m.local.isEmpty() && screen == Screen.CHAT) screen = Screen.MODELS
    }
    Scaffold(
        bottomBar = {
            // The status line sits on every screen, above the bar a thumb reaches: one place, one look.
            Column {
                EngineStatusLine(onOpenModels = { screen = Screen.MODELS }, onOpenSetting = openSetting)
                NavigationBar {
                    Screen.values().forEach { s ->
                        NavigationBarItem(
                            selected = screen == s,
                            onClick = { screen = s; page = null },
                            icon = { Icon(s.icon, contentDescription = s.label) },
                            label = { Text(s.label) },
                        )
                    }
                }
            }
        },
    ) { padding ->
        val m = Modifier.padding(padding)
        val back = { page = null }
        when (page) {
            AgentPage.WORKSPACES -> WorkspacesScreen(m, onBack = back)
            AgentPage.SKILLS -> SkillsScreen(m, onBack = back)
            AgentPage.TASKS -> TasksScreen(m, onBack = back, onOpenConversation = { Agent.open(it); page = null; screen = Screen.CHAT })
            AgentPage.PROVIDERS -> ProvidersScreen(m, onBack = back)
            null -> when (screen) {
                Screen.CHAT -> ChatScreen(
                    m,
                    AgentNav(
                        openModels = { screen = Screen.MODELS },
                        openSetting = openSetting,
                        openWorkspaces = { page = AgentPage.WORKSPACES },
                        openSkills = { page = AgentPage.SKILLS },
                        openTasks = { page = AgentPage.TASKS },
                    ),
                )
                Screen.MODELS -> ModelsScreen(m, onOpenChat = { screen = Screen.CHAT })
                Screen.PLAN -> PlanScreen(m, onOpenSetting = openSetting)
                Screen.SETTINGS -> SettingsScreen(m, target = settingTarget, onLanded = { settingTarget = "" }, onOpenAgent = { page = it })
                Screen.METRICS -> MetricsScreen(m)
            }
        }
    }
}
