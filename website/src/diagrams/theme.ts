// Mermaid theme for the amber CRT aesthetic.
//
// Mermaid renders client-side, so the theme is expressed as themeVariables
// rather than CSS: every colour below mirrors a token in styles/global.css so
// diagrams read as phosphor schematics on the same black glass as the rest of
// the site. Keep this in sync with :root when the palette moves.
//
// Mermaid is a large dependency (~165 KB gzip of core before any diagram-type
// chunk), so it is imported lazily: the module is only fetched once a diagram
// approaches the viewport. Pages without a diagram never load it at all.

const fontFamily =
  "'JetBrains Mono', 'IBM Plex Mono', 'Cascadia Mono', 'Courier New', monospace";

const palette = {
  bg: 'transparent',
  panel: '#0a0700',
  panel2: '#120c02',
  text: '#e8cf9c',
  textDim: '#9a7c47',
  amber: '#ffb000',
  amberBright: '#ffcf4d',
  amberDim: '#6b4f1f',
  line: '#33260e',
};

const amberThemeVariables = {
  darkMode: true,
  background: palette.bg,
  fontFamily,
  fontSize: '13px',

  // Flowchart nodes and edges
  primaryColor: palette.panel2,
  primaryTextColor: palette.text,
  primaryBorderColor: palette.amber,
  secondaryColor: palette.panel,
  secondaryTextColor: palette.text,
  secondaryBorderColor: palette.amberDim,
  tertiaryColor: palette.panel,
  tertiaryTextColor: palette.textDim,
  tertiaryBorderColor: palette.line,
  mainBkg: palette.panel2,
  nodeBorder: palette.amber,
  nodeTextColor: palette.text,
  lineColor: palette.textDim,
  textColor: palette.text,
  clusterBkg: palette.panel,
  clusterBorder: palette.line,
  edgeLabelBackground: '#000000',
  titleColor: palette.amberBright,

  // Sequence diagrams
  actorBkg: palette.panel2,
  actorBorder: palette.amber,
  actorTextColor: palette.text,
  actorLineColor: palette.line,
  signalColor: palette.textDim,
  signalTextColor: palette.text,
  labelBoxBkgColor: palette.panel,
  labelBoxBorderColor: palette.amberDim,
  labelTextColor: palette.text,
  loopTextColor: palette.text,
  noteBkgColor: palette.panel,
  noteBorderColor: palette.amberDim,
  noteTextColor: palette.textDim,
  activationBkgColor: palette.panel2,
  activationBorderColor: palette.amber,
  sequenceNumberColor: palette.amberDim,

  // State diagrams
  labelColor: palette.text,
  altBackground: palette.panel,
  transitionColor: palette.textDim,
  transitionLabelColor: palette.textDim,
  stateBkg: palette.panel2,
  stateBorder: palette.amber,
  compositeBackground: palette.panel,
  compositeBorder: palette.line,
};

interface MermaidApi {
  initialize: (config: Record<string, unknown>) => void;
  run: (options: { nodes: HTMLElement[] }) => Promise<unknown>;
}

let initialized = false;

function initialize(mermaid: MermaidApi): void {
  if (initialized) return;
  initialized = true;
  mermaid.initialize({
    startOnLoad: false,
    securityLevel: 'strict',
    theme: 'base',
    themeVariables: amberThemeVariables,
    fontFamily,
    flowchart: {
      htmlLabels: true,
      curve: 'linear',
      useMaxWidth: true,
      padding: 10,
      nodeSpacing: 32,
      rankSpacing: 38,
    },
    sequence: { useMaxWidth: true, mirrorActors: false },
    state: { useMaxWidth: true },
  });
}

// Rendering is serialized through a single queue. mermaid.run mutates shared
// state and is not safe to call concurrently, and IntersectionObserver fires
// several batches in quick succession during a scroll; concurrent calls make
// rendering flaky. One drain loop, one run at a time.
let queue: HTMLElement[] = [];
let draining = false;

function enqueue(nodes: HTMLElement[]): void {
  queue.push(...nodes);
  if (!draining) void drain();
}

// A node is queued at observe time so a second observer (one <Diagram> per
// figure) cannot render the same node twice. A single failure is contained to
// its own figure: mermaid may have already cleared the element, so the source
// is captured first and restored, leaving a readable diagram instead of a
// blank panel.
async function drain(): Promise<void> {
  draining = true;
  try {
    const mermaid = (await import('mermaid')).default;
    initialize(mermaid);
    while (queue.length) {
      const node = queue.shift();
      if (!node) break;
      const source = node.textContent ?? '';
      try {
        await mermaid.run({ nodes: [node] });
      } catch (error) {
        console.error('[diagram] render failed', error);
        node.textContent = source;
        node.classList.add('mermaid-error');
      }
    }
  } finally {
    draining = false;
  }
}

function takePending(selector: string): HTMLElement[] {
  const nodes = Array.from(document.querySelectorAll<HTMLElement>(selector));
  for (const node of nodes) node.dataset.diagramQueued = 'true';
  return nodes;
}

export function observeDiagrams(): void {
  const nodes = takePending('.mermaid:not([data-processed]):not([data-diagram-queued])');
  if (!nodes.length) return;

  if (!('IntersectionObserver' in window)) {
    enqueue(nodes);
    return;
  }

  const observer = new IntersectionObserver(
    (entries) => {
      const visible = entries
        .filter((entry) => entry.isIntersecting)
        .map((entry) => entry.target as HTMLElement);
      if (!visible.length) return;
      for (const node of visible) observer.unobserve(node);
      enqueue(visible);
    },
    { rootMargin: '200px' },
  );
  for (const node of nodes) observer.observe(node);
}
