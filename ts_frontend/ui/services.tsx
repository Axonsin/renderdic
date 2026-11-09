import { createContext, useContext, type ReactNode } from "react";
import type { Services } from "../contracts/services";
const Context = createContext<Services | null>(null);
export function ServiceProvider({
  services,
  children,
}: {
  services: Services;
  children: ReactNode;
}) {
  return <Context.Provider value={services}>{children}</Context.Provider>;
}
export function useServices() {
  const value = useContext(Context);
  if (!value) throw Error("ServiceProvider is required");
  return value;
}
