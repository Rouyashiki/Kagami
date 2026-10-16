let callbackCounter = 0;

function uniqueName(prefix) {
  callbackCounter += 1;
  return `${prefix}_callback_${Date.now()}_${callbackCounter}`;
}

export function hasKernelSU() {
  return typeof globalThis.ksu !== "undefined";
}

export function exec(command, options = {}, timeoutMs = 15000) {
  return new Promise((resolve, reject) => {
    if (!hasKernelSU() || typeof globalThis.ksu?.exec !== "function") {
      reject(new Error("The root command bridge is unavailable"));
      return;
    }

    const callbackName = uniqueName("exec");
    const timer = setTimeout(() => {
      delete globalThis[callbackName];
      const error = new Error("The root command timed out; its result could not be confirmed");
      error.code = "BRIDGE_TIMEOUT";
      error.outcomeUnknown = true;
      reject(error);
    }, timeoutMs);
    globalThis[callbackName] = (errno, stdout, stderr) => {
      clearTimeout(timer);
      delete globalThis[callbackName];
      const code = Number(errno);
      if ((typeof errno !== "number" && typeof errno !== "string") || String(errno).trim() === "" || !Number.isInteger(code)) {
        reject(new Error("The root command bridge returned an invalid exit status"));
        return;
      }
      resolve({ errno: code, stdout: String(stdout ?? ""), stderr: String(stderr ?? "") });
    };

    try {
      globalThis.ksu.exec(command, JSON.stringify(options), callbackName);
    } catch (error) {
      clearTimeout(timer);
      delete globalThis[callbackName];
      reject(error);
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
