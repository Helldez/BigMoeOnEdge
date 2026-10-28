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
import io.bigmoeonedge.app.chat.Chat
import io.bigmoeonedge.app.engine.Engine
import io.bigmoeonedge.app.ui.ChatNav
import io.bigmoeonedge.app.ui.BmoeTheme
import androidx.compose.foundation.layout.Column
import io.bigmoeonedge.app.ui.ChatScreen
import io.bigmoeonedge.app.ui.EngineStatusLine
import io.bigmoeonedge.app.ui.MetricsScreen
import io.bigmoeonedge.app.ui.ModelsScreen
import io.bigmoeonedge.app.ui.PlanScreen
import io.bigmoeonedge.app.ui.SettingsScreen

/**
 * The app: four screens in the bar over the one engine (chat, models, plan, settings), plus metrics,
 * opened from Settings once its recording is on; each a client of bmoe-server on loopback. The server starts
 * with the app (plain chat: `--chat-only`) and stays up in a foreground service; the model it
 * holds is loaded from Models.
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
        Chat.init(this)
        Engine.start(this)
        setContent {
            BmoeTheme {
                Surface(color = MaterialTheme.colorScheme.background) { Root() }
            }
        }
    }
}

/** A screen and the bar item that shows it as current: Metrics has no item of its own, it sits under Settings. */
private enum class Screen(val label: String, val icon: ImageVector?) {
    CHAT("Chat", Icons.Filled.Email),
    MODELS("Models", Icons.AutoMirrored.Filled.List),
    PLAN("Plan", Icons.Filled.Info),
    SETTINGS("Settings", Icons.Filled.Settings),
    METRICS("Metrics", null),
}

/** The bar item lit while [this] is shown. */
private val Screen.barItem: Screen get() = if (this == Screen.METRICS) Screen.SETTINGS else this

@Composable
private fun Root() {
    var screen by rememberSaveable { mutableStateOf(Screen.CHAT) }
    // The setting a link points at (the Plan screen, the context warning): Settings lands on it.
    var settingTarget by rememberSaveable { mutableStateOf("") }
    val openSetting = { key: String -> settingTarget = key; screen = Screen.SETTINGS }
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
                        val icon = s.icon ?: return@forEach
                        NavigationBarItem(
                            selected = screen.barItem == s,
                            onClick = { screen = s },
                            icon = { Icon(icon, contentDescription = s.label) },
                            label = { Text(s.label) },
                        )
                    }
                }
            }
        },
    ) { padding ->
        val m = Modifier.padding(padding)
        when (screen) {
            Screen.CHAT -> ChatScreen(m, ChatNav(openModels = { screen = Screen.MODELS }, openSetting = openSetting))
            Screen.MODELS -> ModelsScreen(m, onOpenChat = { screen = Screen.CHAT })
            Screen.PLAN -> PlanScreen(m, onOpenSetting = openSetting)
            Screen.SETTINGS -> SettingsScreen(m, target = settingTarget, onLanded = { settingTarget = "" }, onOpenMetrics = { screen = Screen.METRICS })
            Screen.METRICS -> MetricsScreen(m, onBack = { screen = Screen.SETTINGS })
        }
    }
}
