package io.bigmoeonedge.app.ui

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.ColorScheme
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Text
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import io.bigmoeonedge.app.R

/**
 * The design tokens (docs/design.md, "Slate + cyan"), the one place this app keeps colour values.
 * Every pair of words over a fill was checked for WCAG AA (4.5:1); the derived tokens exist because
 * the chosen light accent is a fill colour and too light for words: [accentInk] carries it as text
 * and as a filled button, [edge] gives a control's outline 3:1 against its surface.
 */
@Immutable
data class Tokens(
    val bg: Color,
    val surface: Color,
    val surface2: Color,
    val line: Color,
    val edge: Color,
    val text: Color,
    val muted: Color,
    val accent: Color,
    val accentInk: Color,
    val onAccent: Color,
    val accentTint: Color,
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
    bg = Color(0xFFF6F8F9),
    surface = Color(0xFFFFFFFF),
    surface2 = Color(0xFFEEF2F4),
    line = Color(0xFFDFE5E9),
    edge = Color(0xFF7D8A95),
    text = Color(0xFF111820),
    muted = Color(0xFF56636E),
    accent = Color(0xFF0891B2),
    accentInk = Color(0xFF0E7490),
    onAccent = Color(0xFFFFFFFF),
    accentTint = Color(0xFFE6F4F8),
    ok = Color(0xFF157A3C),
    okTint = Color(0xFFEAF6EE),
    warn = Color(0xFFA14A07),
    warnTint = Color(0xFFFDF3E2),
    err = Color(0xFFB91C1C),
    errTint = Color(0xFFFCEBEB),
    // cyan, violet, amber, green, rose, slate
    series = listOf(Color(0xFF0891B2), Color(0xFF7C3AED), Color(0xFFD97706), Color(0xFF16A34A), Color(0xFFE11D48), Color(0xFF64748B)),
)

val DarkTokens = Tokens(
    bg = Color(0xFF0B1014),
    surface = Color(0xFF131A20),
    surface2 = Color(0xFF1A232B),
    line = Color(0xFF222C34),
    edge = Color(0xFF5A6873),
    text = Color(0xFFE6EDF2),
    muted = Color(0xFF93A1AC),
    accent = Color(0xFF22B8D6),
    accentInk = Color(0xFF22B8D6),
    onAccent = Color(0xFF0B1014),
    accentTint = Color(0xFF0F2A33),
    ok = Color(0xFF4ADE80),
    okTint = Color(0xFF12261B),
    warn = Color(0xFFFBBF24),
    warnTint = Color(0xFF2A2111),
    err = Color(0xFFF87171),
    errTint = Color(0xFF2C1517),
    series = listOf(Color(0xFF22B8D6), Color(0xFFA78BFA), Color(0xFFFBBF24), Color(0xFF4ADE80), Color(0xFFFB7185), Color(0xFF94A3B8)),
)

/** The tokens of the current appearance, for what Material's colour roles do not name. */
val LocalTokens = staticCompositionLocalOf { LightTokens }

/** Inter: every word the app shows. Static instances, bundled with their licence. */
val UiFont = FontFamily(
    Font(R.font.inter_regular, FontWeight.Normal),
    Font(R.font.inter_medium, FontWeight.Medium),
    Font(R.font.inter_semibold, FontWeight.SemiBold),
    Font(R.font.inter_bold, FontWeight.Bold),
)

/** JetBrains Mono: anything a person may paste or a machine wrote (code, a path, a key, a diff). */
val CodeFont = FontFamily(
    Font(R.font.jetbrains_mono_regular, FontWeight.Normal),
    Font(R.font.jetbrains_mono_bold, FontWeight.Bold),
)

private fun scheme(t: Tokens, dark: Boolean): ColorScheme {
    val base = if (dark) darkColorScheme() else lightColorScheme()
    return base.copy(
        primary = t.accentInk,
        onPrimary = t.onAccent,
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
        labelLarge = t.labelLarge.ui(FontWeight.Medium),
        labelMedium = t.labelMedium.ui(FontWeight.Medium),
        labelSmall = t.labelSmall.ui(FontWeight.Medium),
    )
}

/** The corner radius of every box: cards, fields, buttons, chips, sheets' top corners. */
val Corner = RoundedCornerShape(8.dp)

private val UiShapes = Shapes(
    extraSmall = RoundedCornerShape(4.dp),
    small = Corner,
    medium = Corner,
    large = Corner,
    extraLarge = RoundedCornerShape(16.dp), // sheets and dialogs: Material's own proportion
)

@Composable
fun BmoeTheme(content: @Composable () -> Unit) {
    val dark = isSystemInDarkTheme()
    val tokens = if (dark) DarkTokens else LightTokens
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

/** The mark: the pixel chip (unchanged) beside the wordmark. */
@Composable
fun BrandMark(wordmark: Boolean = true) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Icon(painterResource(R.drawable.ic_logo), contentDescription = "BigMoeOnEdge", tint = Color.Unspecified, modifier = Modifier.size(24.dp))
        if (wordmark) {
            Spacer(Modifier.width(8.dp))
            Text("BigMoeOnEdge", fontFamily = UiFont, fontWeight = FontWeight.Bold, fontSize = 17.sp, color = MaterialTheme.colorScheme.onSurface)
        }
    }
}
