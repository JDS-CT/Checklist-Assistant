const assert = require("assert");
const { createSaveController } = require("../CHAX-CLIENT/web/save_controller.js");

async function delay(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

async function runTests() {
  const events = [];
  const setState = (id, state) => events.push({ id, state });

  // Simulate a fast save: debounce 50ms, fail 100ms.
  {
    events.length = 0;
    let saved = false;
    const saveFn = async () => {
      await delay(10);
      saved = true;
    };
    const controller = createSaveController(setState, saveFn, 50, 100);
    controller.queue("row-1", () => ({}));
    await delay(20);
    assert(events.some((e) => e.state === "pending"), "Should enter pending state quickly");
    await delay(60); // past debounce
    assert(saved, "Should call saveFn after debounce");
    await delay(80); // past fail window
    assert(events.some((e) => e.state === "saved"), "Should reach saved state");
  }

  // Simulate a slow save that runs longer than fail timer: expect pending warning then saved.
  {
    events.length = 0;
    let saveCalls = 0;
    const saveFn = async () => {
      saveCalls += 1;
      await delay(120); // longer than failMs below
    };
    const controller = createSaveController(setState, saveFn, 30, 60);
    controller.queue("row-2", () => ({}));
    await delay(70); // after fail timer
    assert(events.some((e) => e.state === "pending"), "Should stay pending when save exceeds failMs");
    await delay(80); // after save resolves
    assert(saveCalls === 1, "Should have invoked saveFn once");
    assert(events.some((e) => e.state === "saved"), "Should eventually reach saved after slow save finishes");
  }

  console.log("save_controller_test ok");
}

runTests().catch((err) => {
  console.error("save_controller_test failure:", err);
  process.exit(1);
});
