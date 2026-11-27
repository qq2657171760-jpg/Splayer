import { createContext, useCallback, useContext, useEffect, useMemo, useState } from "react";

export type AppLanguage = "cn" | "en";

const STORAGE_KEY = "splayer-language";
const originalText = new WeakMap<Node, { source: string; translated: string }>();
const originalAttributes = new WeakMap<Element, Map<string, { source: string; translated: string }>>();

const messages: Record<string, { cn: string; en: string }> = {
  "Settings": { cn: "设置", en: "Settings" },
  "Interface language": { cn: "界面语言", en: "Interface language" },
  "Choose the language used throughout Splayer.": { cn: "选择 Splayer 全局界面显示的语言。", en: "Choose the language used throughout Splayer." },
  "Simplified Chinese": { cn: "简体中文", en: "Simplified Chinese" },
  "English": { cn: "英语", en: "English" },
  "Folders": { cn: "文件夹", en: "Folders" },
  "Downloads": { cn: "下载", en: "Downloads" },
  "Behavior": { cn: "行为", en: "Behavior" },
  "Buttons": { cn: "按键", en: "Buttons" },
  "Backup": { cn: "备份", en: "Backup" },
  "Audio": { cn: "音频", en: "Audio" },
  "Take the tour": { cn: "查看使用引导", en: "Take the tour" },
  "Hide mini player": { cn: "隐藏迷你播放器", en: "Hide mini player" },
  "Show mini player": { cn: "显示迷你播放器", en: "Show mini player" },
  "Music Library Folder": { cn: "音乐库文件夹", en: "Music Library Folder" },
  "The folder where your local music files live.": { cn: "存放本地音乐文件的文件夹。", en: "The folder where your local music files live." },
  "Downloads Folder": { cn: "下载文件夹", en: "Downloads Folder" },
  "Where downloaded YouTube audio files are saved to disk.": { cn: "YouTube 下载的音频文件保存位置。", en: "Where downloaded YouTube audio files are saved to disk." },
  "Videos Folder": { cn: "视频文件夹", en: "Videos Folder" },
  "Where downloaded YouTube video files (MP4) are saved to disk.": { cn: "YouTube 下载的视频文件（MP4）保存位置。", en: "Where downloaded YouTube video files (MP4) are saved to disk." },
  "Backup Folder": { cn: "备份文件夹", en: "Backup Folder" },
  "Where automatic and manual backups are saved.": { cn: "自动和手动备份文件的保存位置。", en: "Where automatic and manual backups are saved." },
  "Reset to defaults (~/Music/Splayer/)": { cn: "恢复默认路径（~/Music/Splayer/）", en: "Reset to defaults (~/Music/Splayer/)" },
  "No folder selected": { cn: "未选择文件夹", en: "No folder selected" },
  "Choose a folder": { cn: "选择文件夹", en: "Choose a folder" },
  "Folder picker is only available in the desktop app": { cn: "文件夹选择器仅在桌面版中可用", en: "Folder picker is only available in the desktop app" },
  "Change": { cn: "更改", en: "Change" },
  "Saved": { cn: "已保存", en: "Saved" },
  "When you click the ✕ button": { cn: "点击 ✕ 关闭按钮时", en: "When you click the ✕ button" },
  "Choose what happens when you close the app window.": { cn: "选择关闭应用窗口时的操作。", en: "Choose what happens when you close the app window." },
  "Minimize to system tray": { cn: "最小化到系统托盘", en: "Minimize to system tray" },
  "The app keeps running in the background and music keeps playing. Click the tray icon to reopen.": { cn: "应用将在后台继续运行并保持播放。点击托盘图标可重新打开。", en: "The app keeps running in the background and music keeps playing. Click the tray icon to reopen." },
  "Close fully": { cn: "完全退出", en: "Close fully" },
  "The app shuts down completely and music stops.": { cn: "完全退出应用并停止播放。", en: "The app shuts down completely and music stops." },
  "This setting only applies in the desktop app.": { cn: "此设置仅适用于桌面版。", en: "This setting only applies in the desktop app." },
  "Export Backup": { cn: "导出备份", en: "Export Backup" },
  "Save all your playlists, settings, themes, and preferences as a single JSON file you can restore later.": { cn: "将播放列表、设置、主题和偏好保存为一个 JSON 文件，以便日后恢复。", en: "Save all your playlists, settings, themes, and preferences as a single JSON file you can restore later." },
  "Exporting…": { cn: "正在导出…", en: "Exporting…" },
  "📤 Export Backup": { cn: "📤 导出备份", en: "📤 Export Backup" },
  "Import Backup": { cn: "导入备份", en: "Import Backup" },
  "Restore from a previously exported Splayer backup file. Your current data will be replaced.": { cn: "从之前导出的 Splayer 备份文件恢复。当前数据将被替换。", en: "Restore from a previously exported Splayer backup file. Your current data will be replaced." },
  "Importing…": { cn: "正在导入…", en: "Importing…" },
  "📥 Import Backup": { cn: "📥 导入备份", en: "📥 Import Backup" },
  "Auto Backup": { cn: "自动备份", en: "Auto Backup" },
  "Automatically save backups to your Downloads folder on a schedule.": { cn: "按计划自动将备份保存到下载文件夹。", en: "Automatically save backups to your Downloads folder on a schedule." },
  "Frequency:": { cn: "备份频率：", en: "Frequency:" },
  "Every 3 days": { cn: "每 3 天", en: "Every 3 days" },
  "Every 7 days": { cn: "每 7 天", en: "Every 7 days" },
  "Every 14 days": { cn: "每 14 天", en: "Every 14 days" },
  "Last backup:": { cn: "上次备份：", en: "Last backup:" },
  "Never": { cn: "从未备份", en: "Never" },
  "⚠️ Set your Downloads folder in the Folders tab for auto backup to work.": { cn: "⚠️ 请先在“文件夹”选项卡设置下载目录，自动备份才能正常工作。", en: "⚠️ Set your Downloads folder in the Folders tab for auto backup to work." },
  "Saving…": { cn: "正在保存…", en: "Saving…" },
  "Back up now": { cn: "立即备份", en: "Back up now" },
  "Start on boot": { cn: "开机启动", en: "Start on boot" },
  "Automatically launch Splayer when you log in.": { cn: "登录系统时自动启动 Splayer。", en: "Automatically launch Splayer when you log in." },
  "Start on boot only works in the desktop app.": { cn: "开机启动仅在桌面版中可用。", en: "Start on boot only works in the desktop app." },
  "OS Media Integration": { cn: "系统媒体集成", en: "OS Media Integration" },
  "Show current track in GNOME / KDE media widget (MPRIS). Responds to media keys from the OS.": { cn: "在 GNOME / KDE 媒体控件中显示当前曲目（MPRIS），并响应系统媒体按键。", en: "Show current track in GNOME / KDE media widget (MPRIS). Responds to media keys from the OS." },
  "Show current track in the Windows media popup (SMTC). Responds to volume-key media controls.": { cn: "在 Windows 媒体弹窗中显示当前曲目（SMTC），并响应系统媒体控制按键。", en: "Show current track in the Windows media popup (SMTC). Responds to volume-key media controls." },
  "Shows track info and responds to OS media keys (MPRIS on Linux, SMTC on Windows).": { cn: "显示曲目信息并响应系统媒体按键（Linux 使用 MPRIS，Windows 使用 SMTC）。", en: "Shows track info and responds to OS media keys (MPRIS on Linux, SMTC on Windows)." },
  "OS media integration only works in the desktop app.": { cn: "系统媒体集成仅在桌面版中可用。", en: "OS media integration only works in the desktop app." },
  "Discord Rich Presence": { cn: "Discord 游戏状态", en: "Discord Rich Presence" },
  "Show the currently playing song on your Discord profile. Requires Discord to be running.": { cn: "在 Discord 个人资料中显示当前播放的歌曲。需要 Discord 正在运行。", en: "Show the currently playing song on your Discord profile. Requires Discord to be running." },
  "Discord Rich Presence only works in the desktop app.": { cn: "Discord 游戏状态仅在桌面版中可用。", en: "Discord Rich Presence only works in the desktop app." },
  "Reset to defaults": { cn: "恢复默认设置", en: "Reset to defaults" },
  "Join our Discord": { cn: "加入 Discord 社区", en: "Join our Discord" },
  "下载记录": { cn: "下载记录", en: "下载记录" },
  "应用行为": { cn: "应用行为", en: "应用行为" },
  "系统媒体": { cn: "系统媒体", en: "系统媒体" },
  "使用引导": { cn: "使用引导", en: "使用引导" },
  "播放控制": { cn: "播放控制", en: "播放控制" },
  "备份与恢复": { cn: "备份与恢复", en: "备份与恢复" },
  "自动音频输出": { cn: "自动音频输出", en: "自动音频输出" },
  "输出设备能力探测": { cn: "输出设备能力探测", en: "输出设备能力探测" },
  "读取活动输出端点的共享格式和可用独占 PCM 格式": { cn: "读取当前活动输出设备的共享格式及支持的独占 PCM 格式", en: "读取活动输出端点的共享格式和可用独占 PCM 格式" },
  "检测设备": { cn: "检测设备", en: "检测设备" },
  "探测中…": { cn: "正在探测…", en: "探测中…" },
  "WASAPI 仅支持 Windows 桌面版。": { cn: "WASAPI 仅支持 Windows 桌面版。", en: "WASAPI 仅支持 Windows 桌面版。" },
  "没有检测到 WASAPI 输出设备": { cn: "未检测到 WASAPI 输出设备", en: "没有检测到 WASAPI 输出设备" },
  "Windows 没有返回活动的音频输出端点。": { cn: "Windows 未返回活动的音频输出端点。", en: "Windows 没有返回活动的音频输出端点。" },
  "系统共享格式：": { cn: "系统共享格式：", en: "系统共享格式：" },
  "独占模式探测通过的 PCM 格式": { cn: "独占模式支持的 PCM 格式", en: "独占模式探测通过的 PCM 格式" },
  "未探测到通用 16 / 24 / 32 位 PCM 格式；播放时将自动回退共享模式。": { cn: "未检测到常见的 16 / 24 / 32 位 PCM 格式；播放时将自动回退到共享模式。", en: "未探测到通用 16 / 24 / 32 位 PCM 格式；播放时将自动回退共享模式。" },
};

export function getInitialLanguage(): AppLanguage {
  try {
    return localStorage.getItem(STORAGE_KEY) === "en" ? "en" : "cn";
  } catch {
    return "cn";
  }
}

export function translate(language: AppLanguage, message: string): string {
  return messages[message]?.[language] ?? message;
}

interface I18nContextValue {
  language: AppLanguage;
  setLanguage: (language: AppLanguage) => void;
  t: (message: string) => string;
}

const I18nContext = createContext<I18nContextValue | null>(null);

export function I18nProvider({ children }: { children: React.ReactNode }) {
  const [language, setLanguageState] = useState<AppLanguage>(getInitialLanguage);

  const setLanguage = useCallback((next: AppLanguage) => {
    setLanguageState(next);
    try { localStorage.setItem(STORAGE_KEY, next); } catch {}
    document.documentElement.lang = next === "cn" ? "zh-CN" : "en";
    window.dispatchEvent(new CustomEvent("splayer-language-change", { detail: next }));
  }, []);

  useEffect(() => {
    document.documentElement.lang = language === "cn" ? "zh-CN" : "en";
    const localizeNode = (node: Node) => {
      if (node.nodeType === Node.TEXT_NODE) {
        const current = node.textContent ?? "";
        const previous = originalText.get(node);
        const source = previous && current === previous.translated ? previous.source : current;
        const trimmed = source.trim();
        const translated = language === "cn" ? messages[trimmed]?.cn : undefined;
        if (translated && translated !== trimmed) {
          const leading = source.match(/^\\s*/)?.[0] ?? "";
          const trailing = source.match(/\\s*$/)?.[0] ?? "";
          const next = leading + translated + trailing;
          originalText.set(node, { source, translated: next });
          if (current !== next) node.textContent = next;
        } else if (previous && current === previous.translated && language === "en") {
          originalText.set(node, { source, translated: source });
          if (current !== source) node.textContent = source;
        }
        return;
      }
      if (node.nodeType !== Node.ELEMENT_NODE) return;
      const element = node as Element;
      let saved = originalAttributes.get(element);
      if (!saved) {
        saved = new Map();
        originalAttributes.set(element, saved);
      }
      for (const attribute of ["title", "placeholder", "aria-label"]) {
        const current = element.getAttribute(attribute);
        if (current === null) continue;
        const previous = saved.get(attribute);
        const source = previous && current === previous.translated ? previous.source : current;
        const translated = language === "cn" ? messages[source]?.cn : undefined;
        if (translated && translated !== source) {
          saved.set(attribute, { source, translated });
          if (current !== translated) element.setAttribute(attribute, translated);
        } else if (previous && current === previous.translated && language === "en") {
          saved.set(attribute, { source, translated: source });
          if (current !== source) element.setAttribute(attribute, source);
        }
      }
      for (const child of Array.from(node.childNodes)) localizeNode(child);
    };
    localizeNode(document.body);
    const observer = new MutationObserver((records) => {
      for (const record of records) {
        if (record.type === "characterData") localizeNode(record.target);
        else record.addedNodes.forEach(localizeNode);
        if (record.type === "attributes") localizeNode(record.target);
      }
    });
    observer.observe(document.body, { subtree: true, childList: true, characterData: true, attributes: true, attributeFilter: ["title", "placeholder", "aria-label"] });
    const handleChange = (event: Event) => {
      const next = (event as CustomEvent<AppLanguage>).detail;
      if (next === "cn" || next === "en") setLanguageState(next);
    };
    window.addEventListener("splayer-language-change", handleChange);
    return () => {
      observer.disconnect();
      window.removeEventListener("splayer-language-change", handleChange);
    };
  }, [language]);

  const value = useMemo<I18nContextValue>(() => ({
    language,
    setLanguage,
    t: (message) => translate(language, message),
  }), [language, setLanguage]);

  return <I18nContext.Provider value={value}>{children}</I18nContext.Provider>;
}

export function useI18n() {
  const context = useContext(I18nContext);
  if (!context) throw new Error("useI18n must be used inside I18nProvider");
  return context;
}
