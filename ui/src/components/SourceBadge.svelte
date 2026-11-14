<script lang="ts">
  import { t } from '../lib/text';

  let { source, reason = '' }: { source: string; reason?: string } = $props();
  const tone: Record<string, string> = { measured: 'ok', derived: 'info', policy: '', operator: 'warn', unprobed: 'err' };
</script>

<span class="badge {tone[source] ?? ''}" title={reason}>{t('source.badge', { source })}</span>
