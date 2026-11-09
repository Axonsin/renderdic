import { create } from "zustand";
import { persist } from "zustand/middleware";
interface Appearance {
  theme: "system" | "dark" | "light";
  fontSize: number;
  codeSize: number;
  left: number;
  right: number;
  set: (change: Partial<Omit<Appearance, "set">>) => void;
}
export const useAppearance = create<Appearance>()(
  persist(
    (set) => ({
      theme: "system",
      fontSize: 14,
      codeSize: 14,
      left: 300,
      right: 300,
      set: (change) => set(change),
    }),
    { name: "renderdic-ts-appearance-v1" },
  ),
);
