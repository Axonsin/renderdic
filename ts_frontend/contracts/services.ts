export interface EventRow {
  eventId: number;
  parent: number | null;
  depth: number;
  name: string;
  flags: string;
  children: number;
  draw: boolean;
}
export interface Texture {
  id: string;
  name: string;
  width: number;
  height: number;
  format: string;
  mips: number;
  arraysize: number;
}
export interface Stage {
  stage: string;
  id: string;
  name: string;
}
export interface Selection {
  generation: number;
  eventId: number;
  name: string;
  stages: Stage[];
  outputs: Texture[];
  depth: Texture | null;
  textureId: string | null;
  numIndices: number;
  numInstances: number;
  elapsedMs: number;
}
export interface Capture {
  path: string;
  generation: number;
  api: string;
  events: EventRow[];
  textures: Texture[];
  initial: Selection;
  elapsedMs: number;
}
export interface Shader {
  text: string;
  kind: string;
  stage: string;
  eventId: number;
  generation: number;
}
export interface Viewport {
  x: number;
  y: number;
  width: number;
  height: number;
  scale: number;
  visible: boolean;
}
export interface Notice {
  event: string;
  value?: number;
  message?: string;
}
export interface Services {
  chooseCapture(): Promise<string | null>;
  openCapture(path: string): Promise<Capture>;
  closeCapture(): Promise<void>;
  selectEvent(eventId: number, generation: number): Promise<Selection>;
  getShader(
    stage: string,
    generation: number,
    eventId: number,
  ): Promise<Shader>;
  setTexture(
    textureId: string,
    generation: number,
    eventId: number,
    fit: boolean,
    channels: boolean[],
  ): Promise<{ textureId: string }>;
  setWindowTheme(dark: boolean): Promise<void>;
  viewport(bounds: Viewport): Promise<void>;
  restart(): Promise<void>;
  onNotice(callback: (notice: Notice) => void): () => void;
}
declare global {
  interface Window {
    renderdic: Services;
    frontendTest?: {
      call(method: string, params?: unknown): Promise<any>;
      window(action: string): Promise<any>;
    };
  }
}
