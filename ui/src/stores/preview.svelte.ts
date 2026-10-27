// The preview pane (docs/screens.md, "Layout"): which workspace file of which conversation it
// shows, and how. Opened from a tool card or the conversation's header; one file at a time.
class PreviewStore {
  conversation = $state('');
  path = $state('');
  open = $state(false);
  fullscreen = $state(false);
  /** Bumped by Refresh: the pane reads the file again. */
  revision = $state(0);

  show(conversation: string, path: string): void {
    this.conversation = conversation;
    this.path = path;
    this.open = true;
    this.revision++;
  }
  refresh(): void {
    this.revision++;
  }
  close(): void {
    this.open = false;
    this.fullscreen = false;
  }
}

export const preview = new PreviewStore();
