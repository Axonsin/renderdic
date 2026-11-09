import type { EventRow } from "./services";
// A filter retains ancestor context. Collapse applies only when not filtering.
export function visibleEvents(
  rows: EventRow[],
  query: string,
  collapsed: Set<number>,
): EventRow[] {
  const text = query.trim().toLocaleLowerCase();
  if (text) {
    const byId = new Map(rows.map((r) => [r.eventId, r]));
    const keep = new Set<number>();
    for (const row of rows)
      if (`${row.eventId} ${row.name}`.toLocaleLowerCase().includes(text)) {
        let current: EventRow | undefined = row;
        while (current && !keep.has(current.eventId)) {
          keep.add(current.eventId);
          current =
            current.parent == null ? undefined : byId.get(current.parent);
        }
      }
    return rows.filter((r) => keep.has(r.eventId));
  }
  let hiddenDepth = Infinity;
  return rows.filter((row) => {
    if (row.depth > hiddenDepth) return false;
    hiddenDepth = collapsed.has(row.eventId) ? row.depth : Infinity;
    return true;
  });
}
export class Revision {
  private current = 0;
  next() {
    return ++this.current;
  }
  isCurrent(revision: number) {
    return revision === this.current;
  }
}
