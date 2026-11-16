package io.bigmoeonedge.app.ui

import android.app.Activity
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.ButtonColors
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.ColorScheme
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.IconButtonColors
import androidx.compose.material3.IconButtonDefaults
import androidx.compose.material3.TopAppBarColors
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Text
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.ExperimentalTextApi
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontVariation
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.view.WindowCompat
import io.bigmoeonedge.app.R

/**
 * The design tokens ("Letterhead", the web UI's `ui/src/app.css`), the one place this app keeps
 * colour values. Words sit on white sheets over a cool grey desk; the ink of the words is also the
 * navigation's shell and the one primary button of a screen. [accentInk] carries links, focus and
 * selection marks; [edge] is a field's outline (3:1 against its surface); [seal] is the logo's
 * yellow, used only as the rule under the logo on the welcome sheet.
 */
@Immutable
data class Tokens(
    val bg: Color,
    val surface: Color,
    val surface2: Color,
    val line: Color,
    /** line-strong: a field's or a control's edge. */
    val edge: Color,
    val text: Color,
    val muted: Color,
    val accent: Color,
    val accentInk: Color,
    val accentTint: Color,
    /** The one primary button of a screen (Send, Load, Apply) and the words on it. */
    val primaryFill: Color,
    val onPrimary: Color,
    /** The navigation drawer and the top app bar, the selected row in it, and the words on them. */
    val shell: Color,
    val shellHi: Color,
    val onShell: Color,
    val onShellMuted: Color,
    val seal: Color,
    val ok: Color,
    val okTint: Color,
    val warn: Color,
    val warnTint: Color,
    val err: Color,
    val errTint: Color,
    /**
     * Categorical colours for the parts of one whole (a memory bar's segments): six hues far apart,
     * so neighbours never read as one, each kept clear of [line] on the surface. Index i % size.
     */
    val series: List<Color>,
)

val LightTokens = Tokens(
    bg = Color(0xFFF3F4F8),
    surface = Color(0xFFFFFFFF),
    surface2 = Color(0xFFECEEF4),
    line = Color(0xFFDCDFEA),
    edge = Color(0xFF6E7396),
    text = Color(0xFF1B1F3A),
    muted = Color(0xFF555A7C),
    accent = Color(0xFF2F3A8F),
    accentInk = Color(0xFF2F3A8F),
    accentTint = Color(0xFFE6E8F4),
    primaryFill = Color(0xFF1B1F3A),
    onPrimary = Color(0xFFFFFFFF),
    shell = Color(0xFF1B1F3A),
    shellHi = Color(0xFF2C3160),
    onShell = Color(0xFFFFFFFF),
    onShellMuted = Color(0xFFC7CAE0),
    seal = Color(0xFFFFC53D),
    ok = Color(0xFF157A3C),
    okTint = Color(0xFFE6F4EA),
    warn = Color(0xFF9A4A00),
    warnTint = Color(0xFFFFF1D6),
    err = Color(0xFFB42318),
    errTint = Color(0xFFFDECEA),
    // indigo, teal, amber, green, rose, slate
    series = listOf(Color(0xFF2F3A8F), Color(0xFF0B8EA8), Color(0xFFB87400), Color(0xFF2E8B57), Color(0xFFC2416E), Color(0xFF6B6F88)),
)

val DarkTokens = Tokens(
    bg = Color(0xFF10121F),
    surface = Color(0xFF191C30),
    surface2 = Color(0xFF20243C),
    line = Color(0xFF2C3150),
    edge = Color(0xFF7D82A8),
    text = Color(0xFFE8E9F4),
    muted = Color(0xFFA9ADC9),
    accent = Color(0xFF8E9BFF),
    accentInk = Color(0xFFAAB4FF),
    accentTint = Color(0xFF23284A),
    primaryFill = Color(0xFFE8E9F4),
    onPrimary = Color(0xFF10121F),
    shell = Color(0xFF0B0D18),
    shellHi = Color(0xFF23284A),
    onShell = Color(0xFFFFFFFF),
    onShellMuted = Color(0xFFB7BAD4),
    seal = Color(0xFFFFC53D),
    ok = Color(0xFF4ADE80),
    okTint = Color(0xFF12261B),
    warn = Color(0xFFFBBF24),
    warnTint = Color(0xFF2A2111),
    err = Color(0xFFF87171),
    errTint = Color(0xFF2C1517),
    series = listOf(Color(0xFF8E9BFF), Color(0xFF21E6FF), Color(0xFFFFC53D), Color(0xFF4ADE80), Color(0xFFFB7185), Color(0xFFA7ABC4)),
)

/** The tokens of the current appearance, for what Material's colour roles do not name. */
val LocalTokens = staticCompositionLocalOf { LightTokens }

/**
 * Hanken Grotesk: every word the app shows. One variable file (res/font, its licence in
 * licenses/OFL-HankenGrotesk.txt), instanced at the weights the type scale uses.
 */
@OptIn(ExperimentalTextApi::class)
private fun hanken(weight: FontWeight) =
    Font(R.font.hanken_grotesk, weight, variationSettings = FontVariation.Settings(FontVariation.weight(weight.weight)))

/** The label weight: between Medium and SemiBold, as the web UI sets its labels (550). */
val LabelWeight = FontWeight(550)

val UiFont = FontFamily(
    hanken(FontWeight.Normal),
    hanken(FontWeight.Medium),
    hanken(LabelWeight),
    hanken(FontWeight.SemiBold),
    hanken(FontWeight.Bold),
)

/** JetBrains Mono: anything a person may paste or a machine wrote (code, a path, a key, a diff). */
val CodeFont = FontFamily(
    Font(R.font.jetbrains_mono_regular, FontWeight.Normal),
    Font(R.font.jetbrains_mono_bold, FontWeight.Bold),
)

private fun scheme(t: Tokens, dark: Boolean): ColorScheme {
    val base = if (dark) darkColorScheme() else lightColorScheme()
    return base.copy(
        // Material's primary is the accent (links, switches, meters, focus); the ink primary button
        // takes its colours from [primaryButtonColors] instead.
        primary = t.accentInk,
        onPrimary = t.surface,
        primaryContainer = t.accentTint,
        onPrimaryContainer = t.text,
        inversePrimary = t.accent,
        secondary = t.muted,
        onSecondary = t.surface,
        secondaryContainer = t.accentTint,
        onSecondaryContainer = t.text,
        tertiary = t.warn,
        onTertiary = t.surface,
        tertiaryContainer = t.warnTint,
        onTertiaryContainer = t.text,
        background = t.bg,
        onBackground = t.text,
        surface = t.bg,
        onSurface = t.text,
        surfaceVariant = t.surface2,
        onSurfaceVariant = t.muted,
        surfaceTint = Color.Transparent, // elevation is a shadow here, never a tinted surface
        surfaceBright = t.surface,
        surfaceDim = t.bg,
        surfaceContainerLowest = t.surface,
        surfaceContainerLow = t.surface,
        surfaceContainer = t.surface,
        surfaceContainerHigh = t.surface2,
        surfaceContainerHighest = t.surface2,
        outline = t.edge,
        outlineVariant = t.line,
        error = t.err,
        onError = t.surface,
        errorContainer = t.errTint,
        onErrorContainer = t.text,
    )
}

private val UiTypography = Typography().let { t ->
    fun TextStyle.ui(weight: FontWeight? = null) = copy(fontFamily = UiFont, fontWeight = weight ?: fontWeight)
    t.copy(
        displayLarge = t.displayLarge.ui(),
        displayMedium = t.displayMedium.ui(),
        displaySmall = t.displaySmall.ui(),
        headlineLarge = t.headlineLarge.ui(FontWeight.SemiBold),
        headlineMedium = t.headlineMedium.ui(FontWeight.SemiBold),
        headlineSmall = t.headlineSmall.ui(FontWeight.SemiBold),
        titleLarge = t.titleLarge.ui(FontWeight.SemiBold),
        titleMedium = t.titleMedium.ui(FontWeight.SemiBold),
        titleSmall = t.titleSmall.ui(FontWeight.SemiBold),
        bodyLarge = t.bodyLarge.ui(),
        bodyMedium = t.bodyMedium.ui(),
        bodySmall = t.bodySmall.ui(),
        labelLarge = t.labelLarge.ui(LabelWeight),
        labelMedium = t.labelMedium.ui(LabelWeight),
        labelSmall = t.labelSmall.ui(LabelWeight),
    )
}

/** The corner of every control: fields, buttons, the composer, a menu. */
val Corner = RoundedCornerShape(10.dp)

/** The corner of a sheet: an answer, a card, the figures of a turn. Lifted by a shadow, never framed. */
val SheetCorner = RoundedCornerShape(6.dp)

/** The person's message: a control's corner, with the one nearest the composer tucked in. */
val BubbleCorner = RoundedCornerShape(topStart = 10.dp, topEnd = 10.dp, bottomEnd = 3.dp, bottomStart = 10.dp)

/** How far a sheet is lifted: a soft shadow, no frame. */
val SheetElevation = 2.dp

private val UiShapes = Shapes(
    extraSmall = RoundedCornerShape(4.dp),
    small = Corner,
    medium = Corner,
    large = SheetCorner,
    extraLarge = RoundedCornerShape(16.dp), // bottom sheets and dialogs: Material's own proportion
)

/** The app's look, [dark] or light; the system bars take the same colours and icons, so they never contradict it. */
@Composable
fun BmoeTheme(dark: Boolean = isSystemInDarkTheme(), content: @Composable () -> Unit) {
    val tokens = if (dark) DarkTokens else LightTokens
    val view = LocalView.current
    if (!view.isInEditMode) {
        SideEffect {
            val window = (view.context as? Activity)?.window ?: return@SideEffect
            // The status bar continues the top app bar's shell, the bottom navigation bar the surface.
            window.statusBarColor = tokens.shell.toArgb()
            window.navigationBarColor = tokens.surface.toArgb()
            WindowCompat.getInsetsController(window, view).apply {
                isAppearanceLightStatusBars = false // the shell is dark in both appearances
                isAppearanceLightNavigationBars = !dark
            }
        }
    }
    CompositionLocalProvider(LocalTokens provides tokens) {
        MaterialTheme(
            colorScheme = scheme(tokens, dark),
            typography = UiTypography,
            shapes = UiShapes,
            content = content,
        )
    }
}

/** The tokens of the current appearance. */
val tokens: Tokens @Composable get() = LocalTokens.current

/** A figure the engine reports: digits of one width, so a live value does not jitter. */
val FigureStyle: TextStyle @Composable get() = TextStyle(fontFamily = UiFont, fontSize = 13.sp, fontFeatureSettings = "tnum")

/** Code, paths, keys and diffs. */
val CodeStyle: TextStyle @Composable get() = TextStyle(fontFamily = CodeFont, fontSize = 13.sp, lineHeight = 18.sp)

/** The ink primary button: the one a screen leads with (Send, Load, Apply). */
@Composable
fun primaryButtonColors(): ButtonColors = ButtonDefaults.buttonColors(containerColor = tokens.primaryFill, contentColor = tokens.onPrimary)

/** Send's colours: the same ink as [primaryButtonColors], for an icon button. */
@Composable
fun primaryIconButtonColors(): IconButtonColors =
    IconButtonDefaults.filledIconButtonColors(containerColor = tokens.primaryFill, contentColor = tokens.onPrimary)

/** Every top app bar: the shell, with its words and icons in on-shell. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun shellTopBarColors(): TopAppBarColors = TopAppBarDefaults.topAppBarColors(
    containerColor = tokens.shell,
    scrolledContainerColor = tokens.shell,
    titleContentColor = tokens.onShell,
    navigationIconContentColor = tokens.onShell,
    actionIconContentColor = tokens.onShell,
)

/** A text action on the shell (a top bar's Refresh, Share): on-shell, where the accent would not read. */
@Composable
fun shellActionColors(): ButtonColors = ButtonDefaults.textButtonColors(contentColor = tokens.onShell)

/** The mark: the pixel chip (unchanged) beside the wordmark, in [colour] (the shell's words by default). */
@Composable
fun BrandMark(wordmark: Boolean = true, colour: Color = tokens.onShell) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Icon(painterResource(R.drawable.ic_logo), contentDescription = "BigMoeOnEdge", tint = Color.Unspecified, modifier = Modifier.size(24.dp))
        if (wordmark) {
            Spacer(Modifier.width(8.dp))
            Text("BigMoeOnEdge", fontFamily = UiFont, fontWeight = FontWeight.Bold, fontSize = 17.sp, color = colour)
        }
    }
}
