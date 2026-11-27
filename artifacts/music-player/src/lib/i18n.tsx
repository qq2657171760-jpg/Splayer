import { createContext, useCallback, useContext, useEffect, useMemo, useState } from "react";

export type AppLanguage = "cn" | "en";

const STORAGE_KEY = "splayer-language";

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
    const handleChange = (event: Event) => {
      const next = (event as CustomEvent<AppLanguage>).detail;
      if (next === "cn" || next === "en") setLanguageState(next);
    };
    window.addEventListener("splayer-language-change", handleChange);
    return () => window.removeEventListener("splayer-language-change", handleChange);
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
