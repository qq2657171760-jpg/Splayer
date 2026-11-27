import { execFileSync, spawnSync } from "node:child_process";
import { existsSync, mkdirSync, unlinkSync, writeFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const source = path.join(root, "native", "wasapi", "wasapi_worker.cpp");
const outputDir = path.join(root, "resources", "wasapi");
const output = path.join(outputDir, "splayer-wasapi.exe");

if (process.platform !== "win32") {
  console.log("[wasapi] Windows native output is built on Windows only; skipping.");
  process.exit(0);
}
if (!existsSync(source)) throw new Error("Missing native WASAPI source: " + source);
mkdirSync(outputDir, { recursive: true });

const common = [
  "/nologo", "/std:c++17", "/EHsc", "/O2", "/MT", "/W4",
  "/DUNICODE", "/D_UNICODE", "/D_WIN32_WINNT=0x0601", "/utf-8",
  source,
  "/link", "ole32.lib", "uuid.lib", "propsys.lib", "avrt.lib",
  "/OUT:" + output,
];

function run(command, args, options = {}) {
  const result = spawnSync(command, args, { stdio: "inherit", windowsHide: true, ...options });
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(command + " failed with exit code " + result.status);
}

// Developer shells may already have cl.exe on PATH.
const clOnPath = spawnSync("where.exe", ["cl.exe"], { encoding: "utf8", windowsHide: true });
if (clOnPath.status === 0) {
  run("cl.exe", common);
} else {
  const vswhere = path.join(process.env["ProgramFiles(x86)"] || "C:\\Program Files (x86)",
    "Microsoft Visual Studio", "Installer", "vswhere.exe");
  if (!existsSync(vswhere)) {
    throw new Error("未检测到 MSVC。请安装 Visual Studio 2022 Build Tools，并勾选“使用 C++ 的桌面开发”和 Windows 10/11 SDK。");
  }
  const installPath = execFileSync(vswhere, [
    "-latest", "-products", "*",
    "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
    "-property", "installationPath"
  ], { encoding: "utf8" }).trim();
  if (!installPath) throw new Error("找不到带 x64 C++ 工具链的 Visual Studio 安装。");
  const devCmd = path.join(installPath, "Common7", "Tools", "VsDevCmd.bat");
  if (!existsSync(devCmd)) throw new Error("找不到 VsDevCmd.bat: " + devCmd);

  // Avoid nested cmd.exe /c quoting: write a temporary batch file so paths
  // containing spaces are parsed by cmd.exe in their normal quoted form.
  const quote = (arg) => '"' + arg.replaceAll('"', '""') + '"';
  const quoted = common.map(quote).join(" ");
  const batchFile = path.join(outputDir, ".build-wasapi-" + process.pid + ".cmd");
  const batch = [
    "@echo off",
    "call " + quote(devCmd) + " -no_logo -arch=x64 -host_arch=x64",
    "if errorlevel 1 exit /b 1",
    "cl.exe " + quoted,
    "exit /b %errorlevel%",
    ""
  ].join("\r\n");
  writeFileSync(batchFile, batch, "utf8");
  try {
    run("cmd.exe", ["/d", "/c", quote(batchFile)]);
  } finally {
    if (existsSync(batchFile)) unlinkSync(batchFile);
  }
}
if (!existsSync(output)) throw new Error("MSVC finished without creating " + output);
console.log("[wasapi] Native worker ready: " + output);
