import { createRoot } from "react-dom/client";
import App from "./App";
import { ThemeProvider } from "./lib/theme-context";
import { I18nProvider } from "./lib/i18n";
import "./index.css";

createRoot(document.getElementById("root")!).render(
  <I18nProvider>
    <ThemeProvider>
      <App />
    </ThemeProvider>
  </I18nProvider>,
);
