<script lang="ts">
  import { t } from '../lib/text';

  // `reason` is accepted for callers that pass it; the row shows it, so the badge does not.
  let { source }: { source: string; reason?: string } = $props();
  const tone: Record<string, string> = { measured: 'ok', derived: 'info', policy: '', operator: 'warn', unprobed: 'err' };
</script>

<!-- How the plan knows the value, in words; the reason itself is in the row it sits on. -->
<span class="badge {tone[source] ?? ''}">{t(`source.${source}`) === `source.${source}` ? source : t(`source.${source}`)}</span>
