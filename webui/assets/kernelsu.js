let callbackCounter = 0;

function uniqueName(prefix) {
  callbackCounter += 1;
  return `${prefix}_callback_${Date.now()}_${callbackCounter}`;
}

export function hasKernelSU() {
  return typeof globalThis.ksu !== "undefined";
}

export function hasAsyncExec() {
  return typeof globalThis.ksu?.spawn === "function";
}

export function exec(command, options = {}, timeoutMs = 15000) {
  return new Promise((resolve, reject) => {
    const asynchronous = hasAsyncExec();
    if (!asynchronous && typeof globalThis.ksu?.exec !== "function") {
      reject(new Error("The root command bridge is unavailable"));
      return;
    }

    const callbackName = uniqueName("exec");
    let settled = false;
    const finish = (error, result) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      delete globalThis[callbackName];
      if (error) reject(error); else resolve(result);
    };
    const timer = setTimeout(() => {
      const error = new Error("The root command timed out; its result could not be confirmed");
      error.code = "BRIDGE_TIMEOUT";
      error.outcomeUnknown = true;
      finish(error);
    }, timeoutMs);
    const complete = (errno, stdout, stderr) => {
      const code = Number(errno);
      if ((typeof errno !== "number" && typeof errno !== "string") || String(errno).trim() === "" || !Number.isInteger(code)) {
        finish(new Error("The root command bridge returned an invalid exit status"));
        return;
      }
      finish(null, { errno: code, stdout: String(stdout ?? ""), stderr: String(stderr ?? "") });
    };

    try {
      if (asynchronous) {
        const stdout = [], stderr = [];
        const stream = lines => ({ emit(event, line) { if (!settled && event === "data") lines.push(String(line ?? "")); } });
        globalThis[callbackName] = {
          stdout: stream(stdout), stderr: stream(stderr),
          emit(event, value) {
            if (settled) return;
            if (event === "exit") complete(value, stdout.join("\n"), stderr.join("\n"));
            else if (event === "error") {
              if (value?.exitCode != null) complete(value.exitCode, stdout.join("\n"), stderr.join("\n") || value.message);
              else finish(new Error(value?.message || "The root command could not be started"));
            }
          },
        };
        // KernelSU evaluates this reference for each stdout/stderr/exit event.
        // A late event (including error after exit) must remain harmless after
        // cleanup, without retaining completed commands on the global object.
        const reference = `(globalThis.${callbackName} || {emit(){},stdout:{emit(){}},stderr:{emit(){}}})`;
        // Pass the already quoted shell command intact, with no extra argv.
        globalThis.ksu.spawn(command, "[]", JSON.stringify(options), reference);
      } else {
        // Old hosts expose only exec. It may block JS despite its callback API;
        // the app disables automatic polling on this compatibility path.
        globalThis[callbackName] = complete;
        globalThis.ksu.exec(command, JSON.stringify(options), callbackName);
      }
    } catch (error) {
      finish(error);
    }
  });
}

export async function enableEdgeToEdge(enabled = true) {
  if (!hasKernelSU()) {
    return false;
  }

  if (typeof globalThis.ksu.enableEdgeToEdge === "function") {
    globalThis.ksu.enableEdgeToEdge(enabled);
    return true;
  }

  if (typeof globalThis.ksu.enableInsets === "function") {
    globalThis.ksu.enableInsets(enabled);
    return true;
  }

  return false;
}

export function toast(message) {
  if (hasKernelSU() && typeof globalThis.ksu.toast === "function") {
    globalThis.ksu.toast(String(message));
    return;
  }

  console.log(`[toast] ${message}`);
}
