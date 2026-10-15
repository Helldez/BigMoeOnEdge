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
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.core.content.ContextCompat
import io.bigmoeonedge.app.engine.Chat
import io.bigmoeonedge.app.engine.Engine
import io.bigmoeonedge.app.ui.BmoeTheme
import io.bigmoeonedge.app.ui.ChatScreen
import io.bigmoeonedge.app.ui.MetricsScreen
import io.bigmoeonedge.app.ui.ModelsScreen
import io.bigmoeonedge.app.ui.PlanScreen
import io.bigmoeonedge.app.ui.SettingsScreen

/**
 * The app: five screens over the one engine, the same five every UI of this project has (chat,
 * models, plan, settings, metrics), each a client of bmoe-server on loopback. The server starts
 * with the app and stays up in a foreground service; the model it holds is loaded from Models.
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
    Scaffold(
        bottomBar = {
            NavigationBar {
                Screen.values().forEach { s ->
                    NavigationBarItem(
                        selected = screen == s,
                        onClick = { screen = s },
                        icon = { Icon(s.icon, contentDescription = s.label) },
                        label = { Text(s.label) },
                    )
                }
            }
        },
    ) { padding ->
        val m = Modifier.padding(padding)
        when (screen) {
            Screen.CHAT -> ChatScreen(m, onOpenModels = { screen = Screen.MODELS })
            Screen.MODELS -> ModelsScreen(m)
            Screen.PLAN -> PlanScreen(m)
            Screen.SETTINGS -> SettingsScreen(m)
            Screen.METRICS -> MetricsScreen(m)
        }
    }
}
