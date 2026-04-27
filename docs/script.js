const translations = {
  en: {
    navGithub: "GitHub",
    languageButton: "PT",
    eyebrow: "Open source Windows folder customization",
    heroTitle: "Give every folder its own color, icon, and personality.",
    heroText:
      "Foldrion helps you organize Windows folders with fast visual recognition using colors, icon packs, and image overlays.",
    downloadCta: "Download latest release",
    sourceCta: "View source",
    versionLabel: "Current version:",
    versionLoading: "Loading...",
    versionUnavailable: "Unavailable",
    heroPoint1: "900+ color options for Windows 7 through 11 folders",
    heroPoint2: "Import `.ico`, `.dll`, `.png`, `.jpg`, and `.jpeg` assets",
    heroPoint3: "No ads, no tracking, no network calls",
    heroImageAlt: "Foldrion folder customization preview",
    featuresKicker: "Why it helps",
    featuresTitle: "Spot the right folder instantly.",
    featuresText:
      "Instead of scanning a long list of names, you can assign meaning through color, branding, or custom image overlays.",
    securityKicker: "Security first",
    securityTitle: "A small native tool with a minimal surface area.",
    securityText:
      "Foldrion is designed as a single executable with embedded resources and no unnecessary online behavior.",
    sectionEyebrow: "Core features",
    sectionTitle: "Built for power users, artists, and tidy desktops.",
    feature1Title: "Huge color library",
    feature1Text:
      "Choose from more than 900 folder color variations across multiple Windows styles.",
    feature2Title: "Bring your own icons",
    feature2Text:
      "Import icon files, image files, and DLL packs, then organize them inside the app.",
    feature3Title: "Create custom combinations",
    feature3Text:
      "Overlay images on colored folders to generate combinations like branded or symbolic folders.",
    feature4Title: "Package icon collections",
    feature4Text:
      "Export many icons as a single DLL pack for easier reuse and sharing.",
    stepsEyebrow: "Quick start",
    stepsTitle: "Three steps to start customizing.",
    step1: "Download `Foldrion.exe`.",
    step2: "Run the app and click Install.",
    step3: "Right-click a folder and choose Customization > Customize Folder.",
    ctaEyebrow: "Free forever",
    ctaTitle: "Use it, inspect it, and adapt it.",
    ctaText:
      "Foldrion is MIT licensed and built for people who want control over their Windows workspace.",
    ctaButton: "Open the repository",
    pageTitle: "Foldrion | Color Your Windows Folders",
    pageDescription:
      "Foldrion is a free open source Windows app to customize folder icons with colors, images, and icon packs."
  },
  pt: {
    navGithub: "GitHub",
    languageButton: "EN",
    eyebrow: "Personalizacao open source de pastas no Windows",
    heroTitle: "De cor, icone e identidade propria para cada pasta.",
    heroText:
      "O Foldrion ajuda voce a organizar pastas no Windows com reconhecimento visual rapido usando cores, pacotes de icones e sobreposicoes de imagem.",
    downloadCta: "Baixar a versao mais recente",
    sourceCta: "Ver codigo-fonte",
    versionLabel: "Versao atual:",
    versionLoading: "Carregando...",
    versionUnavailable: "Indisponivel",
    heroPoint1: "Mais de 900 opcoes de cor para pastas do Windows 7 ao 11",
    heroPoint2: "Importe arquivos `.ico`, `.dll`, `.png`, `.jpg` e `.jpeg`",
    heroPoint3: "Sem anuncios, sem rastreamento, sem chamadas de rede",
    heroImageAlt: "Preview da personalizacao de pastas no Foldrion",
    featuresKicker: "Por que ajuda",
    featuresTitle: "Encontre a pasta certa num instante.",
    featuresText:
      "Em vez de ler uma lista longa de nomes, voce pode atribuir significado visual com cor, marca ou sobreposicoes personalizadas.",
    securityKicker: "Seguranca primeiro",
    securityTitle: "Uma ferramenta nativa pequena, com superficie minima.",
    securityText:
      "O Foldrion foi pensado como um unico executavel com recursos embutidos e sem comportamento online desnecessario.",
    sectionEyebrow: "Recursos principais",
    sectionTitle: "Feito para power users, artistas e desktops organizados.",
    feature1Title: "Biblioteca enorme de cores",
    feature1Text:
      "Escolha entre mais de 900 variacoes de cor de pasta em varios estilos do Windows.",
    feature2Title: "Use seus proprios icones",
    feature2Text:
      "Importe arquivos de icone, imagens e pacotes DLL, depois organize tudo dentro do app.",
    feature3Title: "Crie combinacoes personalizadas",
    feature3Text:
      "Sobreponha imagens em pastas coloridas para gerar combinacoes com marca ou simbolos.",
    feature4Title: "Empacote colecoes de icones",
    feature4Text:
      "Exporte varios icones em um unico pacote DLL para reutilizar e compartilhar com facilidade.",
    stepsEyebrow: "Inicio rapido",
    stepsTitle: "Tres passos para comecar a personalizar.",
    step1: "Baixe o `Foldrion.exe`.",
    step2: "Execute o app e clique em Install.",
    step3: "Clique com o botao direito em uma pasta e escolha Customization > Customize Folder.",
    ctaEyebrow: "Gratis para sempre",
    ctaTitle: "Use, inspecione e adapte.",
    ctaText:
      "Foldrion usa licenca MIT e foi feito para quem quer controle sobre o proprio espaco de trabalho no Windows.",
    ctaButton: "Abrir o repositorio",
    pageTitle: "Foldrion | Colora suas pastas no Windows",
    pageDescription:
      "Foldrion e um app gratuito e open source para Windows que personaliza icones de pasta com cores, imagens e pacotes de icones."
  }
};

const supportedLanguages = ["en", "pt"];
const storedLanguage = window.localStorage.getItem("foldrion-language");
const browserLanguage = (navigator.languages && navigator.languages[0]) || navigator.language || "en";
const versionUrl = "https://raw.githubusercontent.com/zonaro/Foldrion/refs/heads/main/src/Controller/Win32/Release/version.txt";
let currentVersion = null;
let versionLoadFailed = false;

function normalizeLanguage(languageCode) {
  const shortCode = languageCode.toLowerCase().slice(0, 2);
  return supportedLanguages.includes(shortCode) ? shortCode : "en";
}

function updateVersionText(language) {
  const versionElement = document.getElementById("current-version");
  const content = translations[language];

  if (!versionElement) {
    return;
  }

  if (currentVersion) {
    versionElement.textContent = currentVersion;
    return;
  }

  if (versionLoadFailed) {
    versionElement.textContent = content.versionUnavailable;
    return;
  }

  versionElement.textContent = content.versionLoading;
}

function updatePageLanguage(language) {
  const content = translations[language];

  document.documentElement.lang = language;
  document.title = content.pageTitle;

  const description = document.querySelector('meta[name="description"]');
  if (description) {
    description.setAttribute("content", content.pageDescription);
  }

  document.querySelectorAll("[data-i18n]").forEach((element) => {
    const key = element.dataset.i18n;
    if (content[key]) {
      element.textContent = content[key];
    }
  });

  document.querySelectorAll("[data-i18n-alt]").forEach((element) => {
    const key = element.dataset.i18nAlt;
    if (content[key]) {
      element.setAttribute("alt", content[key]);
    }
  });

  updateVersionText(language);
  window.localStorage.setItem("foldrion-language", language);
}

async function loadCurrentVersion() {
  const versionElement = document.getElementById("current-version");

  if (!versionElement) {
    return;
  }

  try {
    const response = await fetch(versionUrl, { cache: "no-store" });
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }

    const versionText = (await response.text()).trim();
    currentVersion = versionText || null;
  } catch (error) {
    versionLoadFailed = true;
  }

  updateVersionText(document.documentElement.lang || "en");
}

const initialLanguage = normalizeLanguage(storedLanguage || browserLanguage);
updatePageLanguage(initialLanguage);
loadCurrentVersion();

document.getElementById("language-switch").addEventListener("click", () => {
  const nextLanguage = document.documentElement.lang === "pt" ? "en" : "pt";
  updatePageLanguage(nextLanguage);
});
