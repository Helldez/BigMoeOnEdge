// Typed client for the agent's routes (docs/agent.md) and the desktop app's (docs/desktop-app.md).
import { auth } from './auth';
import { failed, request } from './api';
import {
  isBackends,
  isConnectClient,
  isConversation,
  isConversationList,
  isConversationSummary,
  isItem,
  isListOf,
  isProvider,
  isSkill,
  isTask,
  isUpdateInfo,
  isWorkspace,
  type Backends,
  type ConnectClient,
  type ConnectWritten,
  type Conversation,
  type ConversationSummary,
  type Item,
  type Mode,
  type Provider,
  type ProviderDraft,
  type Skill,
  type Task,
  type TaskDraft,
  type UpdateDownloaded,
  type UpdateInfo,
  type Workspace,
} from './agent';
import { isObject } from './guards';

const seg = encodeURIComponent;

/** What `POST /api/conversations` takes; `items` carries an imported history (user and assistant only). */
export interface NewConversation {
  workspace?: string | null;
  mode?: Mode;
  route?: string;
  title?: string;
  items?: { kind: 'user' | 'assistant'; text: string; reasoning?: string }[];
}

/** A preview file's bytes, as the server typed them. */
export interface PreviewFile {
  blob: Blob;
  type: string;
}

export const agentApi = {
  workspaces: () => request<Workspace[]>('GET', 'api/workspaces', undefined, isListOf<Workspace>(isWorkspace)),
  addWorkspace: (path: string, name?: string) =>
    request<Workspace>('POST', 'api/workspaces', name ? { path, name } : { path }, isWorkspace),
  removeWorkspace: (id: string) => request<unknown>('DELETE', `api/workspaces/${seg(id)}`),

  conversations: () => request<ConversationSummary[]>('GET', 'api/conversations', undefined, isConversationList),
  createConversation: (body: NewConversation) =>
    request<ConversationSummary>('POST', 'api/conversations', body, isConversationSummary),
  conversation: (id: string) => request<Conversation>('GET', `api/conversations/${seg(id)}`, undefined, isConversation),
  patchConversation: (id: string, body: { title?: string; mode?: Mode; route?: string; workspace?: string | null }) =>
    request<unknown>('PATCH', `api/conversations/${seg(id)}`, body),
  deleteConversation: (id: string) => request<unknown>('DELETE', `api/conversations/${seg(id)}`),
  send: (id: string, text: string, think?: boolean) =>
    request<{ item: Item }>(
      'POST',
      `api/conversations/${seg(id)}/messages`,
      think === undefined ? { text } : { text, think },
      (v): v is { item: Item } => isObject(v) && isItem(v.item),
    ),
  edit: (id: string, item: string, text: string) =>
    request<unknown>('POST', `api/conversations/${seg(id)}/edit`, { item, text }),
  approval: (id: string, item: string, decision: 'approve' | 'deny') =>
    request<unknown>('POST', `api/conversations/${seg(id)}/approval`, { item, decision }),
  stop: (id: string) => request<unknown>('POST', `api/conversations/${seg(id)}/stop`),
  fileUrl: (id: string, path: string) => `api/conversations/${seg(id)}/file?path=${seg(path)}`,
  /** A workspace file for the preview, with the key when there is one (an iframe's own request cannot carry it). */
  async file(id: string, path: string, signal?: AbortSignal): Promise<PreviewFile> {
    const res = await fetch(agentApi.fileUrl(id, path), { headers: auth.headers(), signal });
    if (!res.ok) throw await failed(res);
    const type = res.headers.get('Content-Type') ?? '';
    return { blob: await res.blob(), type };
  },

  skills: () => request<Skill[]>('GET', 'api/skills', undefined, isListOf<Skill>(isSkill)),
  createSkill: (s: { name: string; description: string; body: string }) => request<unknown>('POST', 'api/skills', s),
  updateSkill: (name: string, s: { name: string; description: string; body: string }) =>
    request<unknown>('PUT', `api/skills/${seg(name)}`, s),
  enableSkill: (name: string, enabled: boolean) => request<unknown>('PATCH', `api/skills/${seg(name)}`, { enabled }),
  deleteSkill: (name: string) => request<unknown>('DELETE', `api/skills/${seg(name)}`),

  tasks: () => request<Task[]>('GET', 'api/tasks', undefined, isListOf<Task>(isTask)),
  createTask: (t: TaskDraft) => request<unknown>('POST', 'api/tasks', t),
  updateTask: (id: string, t: TaskDraft) => request<unknown>('PUT', `api/tasks/${seg(id)}`, t),
  deleteTask: (id: string) => request<unknown>('DELETE', `api/tasks/${seg(id)}`),
  runTask: (id: string) => request<unknown>('POST', `api/tasks/${seg(id)}/run`),

  providers: () => request<Provider[]>('GET', 'api/providers', undefined, isListOf<Provider>(isProvider)),
  createProvider: (p: ProviderDraft) => request<unknown>('POST', 'api/providers', p),
  updateProvider: (id: string, p: ProviderDraft) => request<unknown>('PUT', `api/providers/${seg(id)}`, p),
  deleteProvider: (id: string) => request<unknown>('DELETE', `api/providers/${seg(id)}`),

  connect: () => request<ConnectClient[]>('GET', 'api/connect', undefined, isListOf<ConnectClient>(isConnectClient)),
  writeConnect: (id: string) =>
    request<ConnectWritten>('POST', `api/connect/${seg(id)}`, undefined, (v): v is ConnectWritten => isObject(v) && typeof v.file === 'string'),

  backends: () => request<Backends>('GET', 'api/backends', undefined, isBackends),
  installBackend: (id: string) => request<unknown>('POST', `api/backends/${seg(id)}/install`),

  update: () => request<UpdateInfo>('GET', 'api/update', undefined, isUpdateInfo),
  downloadUpdate: () =>
    request<UpdateDownloaded>('POST', 'api/update/download', undefined, (v): v is UpdateDownloaded => isObject(v) && typeof v.path === 'string'),
};
