// Developer-only flow in the actual WebView2 host. The external fixture closes
// the real HWND after confirming a pending production operation over SMB.
import { request } from "./bridge.js";

export async function runSmbFlow(actions, state) {
  await request("prepareSelfTestSmb");
  await actions.reloadSnapshot();
  if (state.scan.state !== "ready") throw new Error("SMB fixture was not scanned");
  if (!(await request("validateDraft")).canCreate) throw new Error("SMB fixture was rejected");
  await request("startCreate");
  await request("smbSelfTestStarted");
  // The native WM_CLOSE / AppService destructor path is the tested completion.
  await new Promise(() => {});
}
