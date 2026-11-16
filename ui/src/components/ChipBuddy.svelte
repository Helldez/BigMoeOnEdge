<script lang="ts">
  // The logo's chip as the engine's status (docs/design.md, "The seal"): a few plain words on what
  // the engine is doing, so a person never needs a figure to know whether it is thinking, reading
  // part of the model from the disk, done, or stuck. Its words are the engine's state and
  // the open conversation's, nothing invented; the figures stay behind "How it ran".
  import { agent } from '../stores/agent.svelte';
  import { server } from '../stores/server.svelte';
  import { t } from '../lib/text';

  let { size = 40, greet = false }: { size?: number; greet?: boolean } = $props();

  type Mood = 'idle' | 'busy' | 'alert' | 'sleep';

  // A share of each token's time spent waiting for the disk above which the chip says so: the
  // moment streaming from flash is what the person is waiting on.
  const DISK_SAYS = 0.3;

  let lastItem = $derived(agent.items.length ? agent.items[agent.items.length - 1] : null);
  let said = $derived.by((): { text: string; mood: Mood } => {
    const info = server.info;
    if (!server.connected) return { text: t('buddy.unreachable'), mood: 'sleep' };
    if (!info) return { text: t('buddy.waking'), mood: 'busy' };
    if (info.state === 'planning') return { text: t('buddy.measuring'), mood: 'busy' };
    if (info.state === 'loading') return { text: t('buddy.loading'), mood: 'busy' };
    if (info.state === 'error') return { text: t('buddy.error'), mood: 'alert' };
    if (agent.current?.state === 'waiting') return { text: t('buddy.needsYou'), mood: 'alert' };
    if (agent.running) {
      const last = server.metrics.last;
      if (last && last.wall_ms > 0 && (last.flash_wait_ms ?? 0) / last.wall_ms > DISK_SAYS) return { text: t('buddy.disk'), mood: 'busy' };
      const writing = lastItem?.kind === 'assistant' && !!lastItem.text;
      return { text: writing ? t('buddy.writing') : t('buddy.thinking'), mood: 'busy' };
    }
    if (info.state === 'empty') return { text: t('buddy.noModel'), mood: 'sleep' };
    if (agent.current?.state === 'error') return { text: t('buddy.failed'), mood: 'alert' };
    if (server.lastDone?.cancelled && lastItem?.kind === 'assistant') return { text: t('buddy.stopped'), mood: 'idle' };
    if (lastItem?.kind === 'assistant') return { text: t('buddy.done'), mood: 'idle' };
    return { text: greet ? t('buddy.hello') : t('buddy.ready'), mood: 'idle' };
  });
</script>

<div class="buddy {said.mood}" class:big={size >= 64} role="status" aria-live="polite">
  <img src="/logo.svg" alt="" width={size} height={size} />
  <p class="bubble">{said.text}</p>
</div>

<style>
  .buddy {
    display: flex;
    align-items: center;
    gap: var(--s2);
    min-width: 0;
  }
  .buddy.big {
    flex-direction: column;
    gap: var(--s3);
  }
  img {
    flex: none;
    image-rendering: pixelated;
  }
  /* Large, the chip sits on a sheet over the seal's yellow rule, as at the head of an answer. */
  .big img {
    padding: 14px;
    border-radius: var(--radius);
    background: var(--surface);
    box-shadow: var(--shadow-md);
    border-bottom: 3px solid var(--seal);
  }
  .bubble {
    margin: 0;
    font-size: 0.875rem;
    font-weight: 550;
    color: var(--muted);
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .big .bubble {
    font-size: 1rem;
    white-space: normal;
    text-align: center;
    max-width: 28rem;
  }
  .alert .bubble {
    color: var(--warn);
  }
  .sleep img {
    filter: grayscale(0.8);
    opacity: 0.7;
  }
  @media (prefers-reduced-motion: no-preference) {
    /* Busy, the chip breathes: the one motion that says "working". */
    .busy img {
      animation: breathe 1.4s ease-in-out infinite alternate;
    }
    @keyframes breathe {
      from {
        opacity: 1;
      }
      to {
        opacity: 0.45;
      }
    }
  }
</style>
