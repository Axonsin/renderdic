import { describe, it, expect } from "vitest";
import { visibleEvents, Revision } from "../contracts/model";
import type { EventRow } from "../contracts/services";
const row = (
  eventId: number,
  parent: number | null,
  depth: number,
  name: string,
  children = 0,
): EventRow => ({
  eventId,
  parent,
  depth,
  name,
  children,
  flags: "",
  draw: !children,
});
const rows = [
  row(1, null, 0, "Frame", 2),
  row(2, 1, 1, "Pass", 1),
  row(3, 2, 2, "DrawIndexed 中文"),
  row(4, 1, 1, "Draw"),
];
describe("event navigation", () => {
  it("retains ancestors of a text match even when collapsed", () =>
    expect(
      visibleEvents(rows, "中文", new Set([1])).map((r) => r.eventId),
    ).toEqual([1, 2, 3]));
  it("collapses only descendants, preserving sibling draws", () =>
    expect(visibleEvents(rows, "", new Set([2])).map((r) => r.eventId)).toEqual(
      [1, 2, 4],
    ));
  it("finds an event by ID", () =>
    expect(visibleEvents(rows, "3", new Set()).map((r) => r.eventId)).toEqual([
      1, 2, 3,
    ]));
  it("ignores stale asynchronous selections", () => {
    const revision = new Revision();
    const old = revision.next();
    const current = revision.next();
    expect(revision.isCurrent(old)).toBe(false);
    expect(revision.isCurrent(current)).toBe(true);
  });
});
