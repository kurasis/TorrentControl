// Runs only with the native host's --self-test switch (also exercised by UI
// tests). Native persistence is checked by an operation registered only there.
import { request } from "./bridge.js";
import { closeDialog, showMagnetCopy } from "./dialogs.js";

async function waitFor(predicate) {
  const deadline = performance.now() + 5000;
  while (!predicate()) {
    if (performance.now() >= deadline) throw new Error("Self-test timed out");
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
}

export async function confirmSelfTestAction(action) {
  const pending = action();
  await waitFor(() => document.getElementById("confirm-yes"));
  document.getElementById("confirm-yes").click();
  await pending;
}

export async function runSelfTest(actions, state, info) {
  const profiles = state.profiles.length;
  const name = "Self-test профиль";
  await actions.saveProfile();
  const input = document.getElementById("profile-name");
  if (!input || !document.getElementById("dialog").open) throw new Error("Profile dialog did not open");
  input.value = name;
  document.getElementById("profile-name-form").requestSubmit();
  await waitFor(() => !document.getElementById("dialog").open);
  const saved = state.profiles.find((profile) => profile.name === name && profile.id === state.draft.profile);
  if (!saved) throw new Error("Profile was not saved through the native bridge");
  const persisted = await request("checkSelfTestProfile", { profileId: saved.id, name });
  if (!persisted.persisted) throw new Error("Saved profile did not survive a settings reload");
  const cancelledDeletion = actions.deleteProfile(saved.id);
  await waitFor(() => document.getElementById("confirm-no"));
  document.getElementById("confirm-no").click();
  await cancelledDeletion;
  if (!(await request("checkSelfTestProfile", { profileId: saved.id, name })).persisted)
    throw new Error("Cancelling deletion removed the saved profile");
  await confirmSelfTestAction(() => actions.deleteProfile(saved.id));
  if ((await request("checkSelfTestProfile", { profileId: saved.id, name })).persisted)
    throw new Error("Confirmed deletion did not remove the saved profile");

  const magnet = "magnet:?xt=urn:btih:0123456789012345678901234567890123456789";
  showMagnetCopy(magnet);
  const text = document.getElementById("magnet-copy-text");
  if (text.value !== magnet || text.selectionStart !== 0 || text.selectionEnd !== magnet.length)
    throw new Error("Manual magnet copy is unavailable");
  closeDialog();
  return { engineVersion: info.engineVersion, appVersion: info.appVersion, profiles,
    profileDialog: true, profilePersisted: true, profileDeleteConfirmation: true, magnetDialog: true };
}
