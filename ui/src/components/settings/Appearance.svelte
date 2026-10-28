<script lang="ts">
  // How the UI looks, kept in this browser: theme, text size, whether Settings shows Metrics.
  import Segmented from './Segmented.svelte';
  import { prefs, TEXT_SCALES, THEMES, type Theme } from '../../stores/prefs.svelte';
  import { t } from '../../lib/text';
</script>

<section class="card section" aria-labelledby="appearance-title">
  <h2 id="appearance-title">{t('settings.appearance')}</h2>
  <div class="setting">
    <div class="what">
      <span class="label" id="theme-label">{t('settings.theme')}</span>
      <span class="muted small">{t('settings.theme.help')}</span>
    </div>
    <Segmented
      label={t('settings.theme')}
      value={prefs.theme}
      options={THEMES.map((v) => ({ value: v, label: t(`settings.theme.${v}`) }))}
      onchange={(v: Theme) => prefs.setTheme(v)}
    />
  </div>
  <div class="setting">
    <div class="what">
      <span class="label">{t('settings.textSize')}</span>
      <span class="muted small">{t('settings.textSize.help')}</span>
    </div>
    <Segmented
      label={t('settings.textSize')}
      value={prefs.textScale}
      options={TEXT_SCALES.map((v) => ({ value: v, label: `${Math.round(v * 100)}%` }))}
      onchange={(v: number) => prefs.setTextScale(v)}
    />
  </div>
  <div class="setting">
    <label class="what" for="metrics">
      <span class="label">{t('settings.metrics')}</span>
      <span class="muted small">{t('settings.metrics.help')}</span>
    </label>
    <input id="metrics" type="checkbox" class="switch" role="switch" checked={prefs.metrics} onchange={(e) => prefs.setMetrics(e.currentTarget.checked)} />
  </div>
</section>
