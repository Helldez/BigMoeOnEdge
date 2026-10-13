<script lang="ts">
  let { source, reason = '' }: { source: string; reason?: string } = $props();
  const tone: Record<string, string> = { measured: 'ok', derived: 'info', policy: '', operator: 'warn', unprobed: 'err' };
</script>

<span class="badge {tone[source] ?? ''}" title={reason}>plan: {source}</span>
