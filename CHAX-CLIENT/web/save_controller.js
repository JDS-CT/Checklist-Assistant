// Lightweight save controller with debounce + failover timers.
// Works in browser (window.createSaveController) and Node (module.exports).

(function (globalFactory) {
  if (typeof module !== "undefined" && module.exports) {
    module.exports = globalFactory();
  } else {
    window.createSaveController = globalFactory().createSaveController;
  }
})(function () {
  function createSaveController(setState, saveFn, debounceMs = 3000, failMs = 5000) {
    const timers = new Map();

    function clearTimers(id) {
      const entry = timers.get(id);
      if (!entry) return;
      if (entry.debounce) clearTimeout(entry.debounce);
      if (entry.failTimer) clearTimeout(entry.failTimer);
    }

    function queue(id, payloadFactory) {
      clearTimers(id);
      setState(id, "pending", "Saving, please wait...");
      const failTimer = setTimeout(() => {
        setState(id, "pending", "Save is taking longer than expected; still trying.");
      }, failMs);

      const debounce = setTimeout(async () => {
        try {
          await saveFn(id, payloadFactory());
          clearTimeout(failTimer);
          setState(id, "saved", "Saved");
        } catch (err) {
          setState(id, "unsaved", err?.message || "Save failed");
        }
      }, debounceMs);

      timers.set(id, { debounce, failTimer });
    }

    return { queue };
  }

  return { createSaveController };
});
