<script lang="ts" module>
  // The UI's icons: line drawings on a 24 px grid, 1.5 px stroke, round caps (docs/design.md,
  // "Geometry"). Drawn for this UI; each is one or more path strings.
  export const ICONS = {
    plus: ['M12 5v14', 'M5 12h14'],
    search: ['M11 18a7 7 0 1 0 0-14 7 7 0 0 0 0 14z', 'M20 20l-4-4'],
    chat: ['M5 5h14a1 1 0 0 1 1 1v10a1 1 0 0 1-1 1H10l-4 3v-3H5a1 1 0 0 1-1-1V6a1 1 0 0 1 1-1z'],
    folder: ['M3.5 7a1.5 1.5 0 0 1 1.5-1.5h4l2 2h8A1.5 1.5 0 0 1 20.5 9v8a1.5 1.5 0 0 1-1.5 1.5H5A1.5 1.5 0 0 1 3.5 17z'],
    clock: ['M12 20.5a8.5 8.5 0 1 0 0-17 8.5 8.5 0 0 0 0 17z', 'M12 7.5V12l3 2'],
    box: ['M12 3.5l7.5 4.25v8.5L12 20.5l-7.5-4.25v-8.5z', 'M4.5 7.75L12 12l7.5-4.25', 'M12 12v8.5'],
    gauge: ['M4 16a8 8 0 1 1 16 0', 'M12 16l3.5-5', 'M4 16h2', 'M18 16h2'],
    spark: ['M12 3.5l1.8 5.2 5.2 1.8-5.2 1.8L12 17.5l-1.8-5.2L5 10.5l5.2-1.8z', 'M18.5 16.5l.7 1.8 1.8.7-1.8.7-.7 1.8-.7-1.8-1.8-.7 1.8-.7z'],
    activity: ['M3.5 12h4l2.5-6 4 12 2.5-6h4'],
    gear: [
      'M12 15a3 3 0 1 0 0-6 3 3 0 0 0 0 6z',
      'M19 12a7 7 0 0 0-.1-1.2l2-1.5-2-3.4-2.3.9a7 7 0 0 0-2.1-1.2L14 3.2h-4l-.5 2.4a7 7 0 0 0-2.1 1.2l-2.3-.9-2 3.4 2 1.5A7 7 0 0 0 5 12c0 .4 0 .8.1 1.2l-2 1.5 2 3.4 2.3-.9c.6.5 1.3.9 2.1 1.2l.5 2.4h4l.5-2.4c.8-.3 1.5-.7 2.1-1.2l2.3.9 2-3.4-2-1.5c.1-.4.1-.8.1-1.2z',
    ],
    plug: ['M9 3.5v4', 'M15 3.5v4', 'M7 7.5h10v3a5 5 0 0 1-10 0z', 'M12 15.5v5'],
    sidebar: ['M4.5 4.5h15a1 1 0 0 1 1 1v13a1 1 0 0 1-1 1h-15a1 1 0 0 1-1-1v-13a1 1 0 0 1 1-1z', 'M9.5 4.5v15'],
    menu: ['M4 7h16', 'M4 12h16', 'M4 17h16'],
    close: ['M6 6l12 12', 'M18 6L6 18'],
    refresh: ['M19.5 12a7.5 7.5 0 1 1-2.2-5.3', 'M19.5 4.5v4h-4'],
    expand: ['M14 4.5h5.5V10', 'M10 19.5H4.5V14', 'M19.5 4.5L13.5 10.5', 'M4.5 19.5l6-6'],
    copy: ['M9 9h9.5a1 1 0 0 1 1 1v9.5a1 1 0 0 1-1 1H9a1 1 0 0 1-1-1V10a1 1 0 0 1 1-1z', 'M5.5 15H5a1 1 0 0 1-1-1V5a1 1 0 0 1 1-1h9a1 1 0 0 1 1 1v.5'],
    check: ['M5 12.5l4.5 4.5L19 7.5'],
    edit: ['M4.5 19.5l1-4L15.8 5.2a1.8 1.8 0 0 1 2.5 0l.5.5a1.8 1.8 0 0 1 0 2.5L8.5 18.5z', 'M13.5 7.5l3 3'],
    stop: ['M7.5 7.5h9v9h-9z'],
    send: ['M12 19V5', 'M6 11l6-6 6 6'],
    chevronDown: ['M6 9l6 6 6-6'],
    chevronRight: ['M9 6l6 6-6 6'],
    chevronLeft: ['M15 6l-6 6 6 6'],
    file: ['M6.5 3.5h7l4 4v12a1 1 0 0 1-1 1h-10a1 1 0 0 1-1-1v-15a1 1 0 0 1 1-1z', 'M13.5 3.5v4h4'],
    terminal: ['M4.5 5h15a1 1 0 0 1 1 1v12a1 1 0 0 1-1 1h-15a1 1 0 0 1-1-1V6a1 1 0 0 1 1-1z', 'M7.5 9.5l3 2.5-3 2.5', 'M12.5 15h4'],
    globe: ['M12 20.5a8.5 8.5 0 1 0 0-17 8.5 8.5 0 0 0 0 17z', 'M3.5 12h17', 'M12 3.5c2.5 2.5 3.5 5.5 3.5 8.5s-1 6-3.5 8.5c-2.5-2.5-3.5-5.5-3.5-8.5s1-6 3.5-8.5z'],
    alert: ['M12 4l8.5 15h-17z', 'M12 10v4', 'M12 16.8v.2'],
    info: ['M12 20.5a8.5 8.5 0 1 0 0-17 8.5 8.5 0 0 0 0 17z', 'M12 11v5', 'M12 8v.2'],
    trash: ['M5 7h14', 'M9.5 7V5h5v2', 'M6.5 7l1 12.5h9l1-12.5', 'M10.5 10.5v6', 'M13.5 10.5v6'],
    external: ['M14 4.5h5.5V10', 'M19.5 4.5L11 13', 'M17 13.5v5a1 1 0 0 1-1 1H5.5a1 1 0 0 1-1-1V8a1 1 0 0 1 1-1h5'],
    download: ['M12 4v11', 'M7.5 10.5L12 15l4.5-4.5', 'M5 19.5h14'],
    play: ['M8 5.5v13l10.5-6.5z'],
    brain: ['M9 4.5a3 3 0 0 0-3 3 3 3 0 0 0-1.5 5.5A3 3 0 0 0 7 18a2.5 2.5 0 0 0 5 .5V6.5a2 2 0 0 0-3-2z', 'M15 4.5a3 3 0 0 1 3 3 3 3 0 0 1 1.5 5.5A3 3 0 0 1 17 18a2.5 2.5 0 0 1-5 .5'],
    shield: ['M12 3.5l7 2.5v5.5c0 4.5-3 7.5-7 9-4-1.5-7-4.5-7-9V6z'],
    lock: ['M7 10.5V8a5 5 0 0 1 10 0v2.5', 'M5.5 10.5h13v9h-13z'],
    eye: ['M2.5 12S6 5.5 12 5.5 21.5 12 21.5 12 18 18.5 12 18.5 2.5 12 2.5 12z', 'M12 14.5a2.5 2.5 0 1 0 0-5 2.5 2.5 0 0 0 0 5z'],
    more: ['M6 12h.01', 'M12 12h.01', 'M18 12h.01'],
    chip: ['M7 7h10v10H7z', 'M9.5 3.5V7', 'M14.5 3.5V7', 'M9.5 17v3.5', 'M14.5 17v3.5', 'M3.5 9.5H7', 'M3.5 14.5H7', 'M17 9.5h3.5', 'M17 14.5h3.5'],
    sun: ['M12 16a4 4 0 1 0 0-8 4 4 0 0 0 0 8z', 'M12 2.5v2', 'M12 19.5v2', 'M2.5 12h2', 'M19.5 12h2', 'M5.3 5.3l1.4 1.4', 'M17.3 17.3l1.4 1.4', 'M5.3 18.7l1.4-1.4', 'M17.3 6.7l1.4-1.4'],
    moon: ['M19.5 14.5A8 8 0 0 1 9.5 4.5a8 8 0 1 0 10 10z'],
    key: ['M8.5 15.5a4 4 0 1 0 0-8 4 4 0 0 0 0 8z', 'M12 11.5h8.5', 'M17.5 11.5v3', 'M20 11.5v2'],
    upload: ['M12 15V4', 'M7.5 8.5L12 4l4.5 4.5', 'M5 19.5h14'],
  } as const;
  export type IconName = keyof typeof ICONS;
</script>

<script lang="ts">
  let { name, size = 18, label = '' }: { name: IconName; size?: number; label?: string } = $props();
</script>

<svg
  class="icon"
  width={size}
  height={size}
  viewBox="0 0 24 24"
  fill="none"
  stroke="currentColor"
  stroke-width="1.5"
  stroke-linecap="round"
  stroke-linejoin="round"
  role={label ? 'img' : undefined}
  aria-label={label || undefined}
  aria-hidden={label ? undefined : 'true'}
  focusable="false"
>
  {#each ICONS[name] as d (d)}<path {d} />{/each}
</svg>

<style>
  .icon {
    flex: none;
    display: inline-block;
    vertical-align: middle;
  }
</style>
