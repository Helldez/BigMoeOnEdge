<script lang="ts">
  // How the UI looks and speaks, kept in this browser: theme, language, text size.
  import Segmented from './Segmented.svelte';
  import { LANG_NAMES, type Lang } from '../../lib/i18n';
  import { prefs, TEXT_SCALES, THEMES, t, type Theme } from '../../stores/prefs.svelte';
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
      <label class="label" for="lang">{t('settings.language')}</label>
      <span class="muted small">{t('settings.language.help')}</span>
    </div>
    <select id="lang" value={prefs.langChoice} onchange={(e) => prefs.setLang(e.currentTarget.value as 'auto' | Lang)}>
      <option value="auto">{t('settings.language.auto')}</option>
      {#each Object.entries(LANG_NAMES) as [code, name] (code)}<option value={code}>{name}</option>{/each}
    </select>
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
</section>
