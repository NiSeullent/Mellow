const REPO = "https://github.com/NiSeullent/Mellow";
const RELEASES_API = "https://api.github.com/repos/NiSeullent/Mellow/releases?per_page=30";
const $ = (selector) => document.querySelector(selector);
const ko = {};
document.querySelectorAll("[data-i18n]").forEach((element) => { ko[element.dataset.i18n] = element.textContent.trim(); });
document.querySelectorAll("[data-i18n-placeholder]").forEach((element) => { ko[element.dataset.i18nPlaceholder] = element.placeholder; });
ko.heroTitle = "내 GPU의 가능성,\n현재 단계부터.";
const en = {
  skip: "Skip to GPU list", navDevices: "GPU list", navDownload: "Download", navInstall: "Install",
  heroTitle: "Your GPU’s potential.\nIts progress, today.",
  heroDescription: "Mellow is a porting project for graphics without macOS drivers. Find your device’s development stage and explore the experimental package.",
  checkGpu: "Find my GPU ↗", getPackage: "Get the package ↓",
  heroNote: "No device has verified native macOS GPU, system Metal and WindowServer support yet.",
  development: "In development", hardwarePending: "Native implementation & validation", unverified: "Unverified",
  sourceSnapshot: "Status source", verificationRecord: "Validation scope ↗", compatTitle: "Where each GPU stands", openData: "Shared data ↗",
  compatDescription: "Search a model, PCI ID or family. Partial code or device identification does not mean completed macOS acceleration support.",
  searchLabel: "Search GPUs", searchPlaceholder: "e.g. RTX 3080, 7D41, Lunar Lake", familyLabel: "Family", stageLabel: "Stage",
  allFamilies: "All families", allStages: "All stages", all: "All", additionalResearch: "Include separate AMD research",
  loadingDevices: "Loading device records…", tableCaption: "Device development stages and evidence",
  deviceColumn: "Device / family", stageColumn: "Development", detailsColumn: "Implemented & remaining", runtimeColumn: "macOS hardware results",
  noResults: "No matching GPU.", noResultsHelp: "Try a different search or filter. A missing entry does not establish support either way.", resetFilters: "Reset filters",
  dataError: "The device list could not be loaded. See the GitHub README for device status.",
  partialLabel: "Partial implementation", identifiedLabel: "Device identification", sourceLabel: "Source review",
  partialExplanation: "Some code, builds and host checks", identifiedExplanation: "PCI identification code", sourceExplanation: "Source paths and recorded gaps",
  scopeNote: "Windows compute/render results use the installed Intel Windows driver. Kernel-object and host checks are separate evidence, not native macOS GPU, Metal or WindowServer hardware success.",
  downloadTitle: "Get the development package", packageDescription: "An experimental binary package for Mellow.kext, the userspace framework and command-line tools.",
  checkingReleases: "Checking public releases for binary assets…", downloadBinary: "Download binary ZIP ↓", allReleases: "View GitHub releases ↗", manifest: "Package manifest", checksum: "SHA-256",
  downloadNote: "Experimental build. Developer ID signing, notarization and device acceleration validation are incomplete. Source ZIPs are not shown as installable binaries. This package does not target Apple Silicon.",
  installTitle: "Install from Terminal", readInstaller: "Read the installer ↗",
  installDescription: "Download and review the installer before running it. The default is ~/Library/Mellow and installs userspace files only.",
  stepDownload: "Download", stepDownloadText: "Save the installer attached to the binary release.", stepReview: "Review", stepReviewText: "Check the selected version and the changes the script makes.",
  stepInstall: "Install", stepInstallText: "Verify the selected release ZIP and SHA-256, then install.", downloadAndReview: "Download & review", runSelectedVersion: "Install selected version",
  copyCommands: "Copy commands", reviewBeforeRun: "Read the saved script, then run the installation command separately.",
  installWaiting: "If binaries are not published yet, Releases still provides development source and validation records.",
  kextDetails: "What about kext installation and hardware validation?",
  kextDetailsText: "The default install does not change system kexts or run GPU acceptance tests. Copying to /Library/Extensions requires --install-kext; installing a required Lilu dependency requires --install-dependencies; standard kmutil preparation requires --prepare-kext. There is no automatic reboot or security-setting change. Success does not establish Metal or WindowServer acceleration support.",
  installDocs: "Installation & validation docs ↗", resourcesTitle: "The work is public, too.", resourceStatus: "Device implementation status ↗", resourceStatusText: "Read the current code and actual validation records together.",
  resourceOwner: "Native GPU development ↗", resourceOwnerText: "Device, memory, firmware and channel ownership — and the remaining integration.",
  resourceMetal: "Metal & window presentation ↗", resourceMetalText: "The explicit app path is tracked separately from system integration.", viewSource: "Project source ↗"
};
let language = new URL(location.href).searchParams.get("lang") === "en" ? "en" : "ko";
let data = null, vendor = "all", deviceState = "loading", releaseState = "loading", releasePackage = null;
const t = (key) => (language === "en" ? en[key] : ko[key]) ?? ko[key] ?? key;
const local = (value) => value[language];
function repoFile(path) {
  return `${REPO}/blob/${encodeURIComponent(data?.source_ref ?? "main")}/${path.split("/").map(encodeURIComponent).join("/")}`;
}
function node(tag, text, className) {
  const element = document.createElement(tag);
  if (text !== undefined) element.textContent = text;
  if (className) element.className = className;
  return element;
}
function applyLanguage() {
  document.documentElement.lang = language;
  document.title = language === "ko" ? "Mellow — GPU 호환성 · 다운로드" : "Mellow — GPU compatibility & downloads";
  document.querySelectorAll("[data-i18n]").forEach((element) => {
    const value = t(element.dataset.i18n);
    if (element.dataset.i18n === "heroTitle") {
      element.replaceChildren(); value.split("\n").forEach((line, i) => { if (i) element.append(document.createElement("br")); element.append(document.createTextNode(line)); });
    } else element.textContent = value;
  });
  document.querySelectorAll("[data-i18n-placeholder]").forEach((element) => { element.placeholder = t(element.dataset.i18nPlaceholder); });
  $("#language").textContent = language === "ko" ? "EN" : "한국어";
  $("#language").setAttribute("aria-label", language === "ko" ? "Switch to English" : "한국어로 변경");
  if (data) { updateSelects(); renderDevices(); }
  else $("#result-count").textContent = deviceState === "error" ? t("dataError") : t("loadingDevices");
  renderRelease();
}
const localizedStrings = (value) => value && typeof value.ko === "string" && typeof value.en === "string";
function validateData(value) {
  if (value?.schema_version !== 1 || value.goal_achieved !== false || !/^[a-f0-9]{40}$/.test(value.source_ref) ||
      !/^\d{4}-\d{2}-\d{2}$/.test(value.updated) || !Array.isArray(value.devices) || !value.devices.length || !value.stages || !value.families)
    throw new Error("Invalid compatibility source contract");
  const ids = new Set();
  for (const entry of value.devices) {
    if (!entry || typeof entry.id !== "string" || !/^[a-z0-9-]+$/.test(entry.id) || ids.has(entry.id) ||
        !["Intel", "NVIDIA", "AMD"].includes(entry.vendor) || typeof entry.name !== "string" || typeof entry.model !== "string" ||
        !localizedStrings(value.stages[entry.stage]) || !localizedStrings(value.families[entry.family]) ||
        !localizedStrings(entry.implemented) || !localizedStrings(entry.remaining) ||
        (entry.taxonomy_source && (typeof value.taxonomy_sources?.[entry.taxonomy_source]?.title !== "string" || !safeSourceUrl(value.taxonomy_sources[entry.taxonomy_source].url))) ||
        (entry.pci_id !== null && !/^[0-9A-F]{4}:[0-9A-F]{4}$/.test(entry.pci_id)) ||
        !Array.isArray(entry.evidence) || !entry.evidence.length || entry.evidence.some((path) =>
          typeof path !== "string" || !/^[A-Za-z0-9._/-]+$/.test(path) || path.startsWith("/") || path.split("/").some((part) => part === ".." || part === ".")) ||
        entry.runtime?.native_gpu !== false || entry.runtime?.metal !== false || entry.runtime?.windowserver !== false)
      throw new Error("Invalid source-only device entry");
    ids.add(entry.id);
  }
  return value;
}
function baseDevices() { return data.devices.filter((entry) => !entry.additional || $("#additional").checked); }
function updateSelects() {
  const family = $("#family"), stage = $("#stage"), oldFamily = family.value, oldStage = stage.value;
  family.replaceChildren(new Option(t("allFamilies"), "all")); stage.replaceChildren(new Option(t("allStages"), "all"));
  const selected = baseDevices().filter((entry) => vendor === "all" || entry.vendor === vendor);
  for (const key of new Set(selected.map((entry) => entry.family))) family.add(new Option(local(data.families[key]), key));
  for (const key of Object.keys(data.stages)) stage.add(new Option(local(data.stages[key]), key));
  if ([...family.options].some((option) => option.value === oldFamily)) family.value = oldFamily;
  if ([...stage.options].some((option) => option.value === oldStage)) stage.value = oldStage;
  $("[data-vendor='AMD']").hidden = !$("#additional").checked;
}
function renderDevices() {
  if (!data) return;
  const query = $("#search").value.trim().toLocaleLowerCase();
  const selected = baseDevices().filter((entry) => {
    const haystack = [entry.vendor, entry.name, entry.model, entry.pci_id ?? "", local(data.families[entry.family]), entry.implemented.ko, entry.implemented.en].join(" ").toLocaleLowerCase();
    return (vendor === "all" || entry.vendor === vendor) && ($("#family").value === "all" || entry.family === $("#family").value) &&
      ($("#stage").value === "all" || entry.stage === $("#stage").value) && haystack.includes(query);
  });
  const fragment = document.createDocumentFragment();
  for (const entry of selected) {
    const row = node("tr"); row.id = entry.id;
    const device = node("td"), stage = node("td"), details = node("td"), runtime = node("td");
    device.append(node("h3", entry.name, "device-name"), node("span", entry.model, "device-model"));
    device.append(node("span", entry.pci_id ?? (language === "ko" ? "기기별 PCI ID 미등록" : "Device-specific PCI ID not registered"), "device-id"));
    stage.append(node("span", local(data.stages[entry.stage]), `stage-pill ${entry.stage}`));
    details.append(node("p", local(entry.implemented), "implemented"), node("p", local(entry.remaining), "remaining"));
    const links = node("div", undefined, "evidence-links");
    entry.evidence.forEach((path, index) => {
      const label = language === "ko" ? (index ? `근거 ${index + 1} ↗` : "구현·검증 근거 ↗") : (index ? `Source ${index + 1} ↗` : "Implementation & evidence ↗");
      const link = node("a", label); link.href = repoFile(path); link.setAttribute("aria-label", `${label}: ${entry.name}, ${path}`); links.append(link);
    });
    if (entry.taxonomy_source) {
      const source = data.taxonomy_sources[entry.taxonomy_source];
      const link = node("a", language === "ko" ? "계열 명칭 출처 ↗" : "Family naming source ↗");
      link.href = safeSourceUrl(source.url); link.setAttribute("aria-label", `${link.textContent}: ${source.title}`); links.append(link);
    }
    details.append(links);
    runtime.append(node("strong", language === "ko" ? "실기 미검증" : "Hardware unverified", "runtime-result"), node("span", "Native GPU · Metal · WindowServer", "runtime-scope"));
    row.append(device, stage, details, runtime); fragment.append(row);
  }
  $("#device-list").replaceChildren(fragment); $("#empty-state").hidden = selected.length !== 0;
  $("#result-count").textContent = language === "ko" ? `${selected.length}개 항목 · 실기 지원 완료 0대` : `${selected.length} entries · 0 devices with completed hardware support`;
  $("#data-date").textContent = (language === "ko" ? "기록 기준 " : "Recorded ") + data.updated;
}
async function fetchJson(url) {
  const controller = new AbortController(), timer = setTimeout(() => controller.abort(), 10000);
  try {
    const response = await fetch(url, { signal: controller.signal, headers: { Accept: "application/json" }, credentials: "omit" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    return await response.json();
  } finally { clearTimeout(timer); }
}
function safeReleaseUrl(value) {
  try {
    const url = new URL(value);
    return url.protocol === "https:" && url.hostname === "github.com" && !url.username && !url.password && url.pathname.startsWith("/NiSeullent/Mellow/releases/") ? url.href : null;
  } catch { return null; }
}
function safeSourceUrl(value) {
  try {
    const url = new URL(value);
    return url.protocol === "https:" && url.hostname === "github.com" && !url.username && !url.password && url.pathname.startsWith("/torvalds/linux/blob/") ? url.href : null;
  } catch { return null; }
}
function releaseAsset(release, name) {
  return release.assets.find((asset) => asset && asset.name === name && asset.state === "uploaded" && Number.isFinite(asset.size) && asset.size > 0 &&
    safeReleaseUrl(asset.browser_download_url) && new URL(asset.browser_download_url).pathname.includes("/releases/download/"));
}
function findBinaryPackage(releases) {
  if (!Array.isArray(releases)) throw new Error("Invalid release response");
  for (const release of releases) {
    if (!release || release.draft || typeof release.tag_name !== "string" || !release.tag_name || !Array.isArray(release.assets)) continue;
    const zip = releaseAsset(release, "Mellow-macos-x86_64.zip"), checksum = releaseAsset(release, "Mellow-macos-x86_64.zip.sha256");
    const manifest = releaseAsset(release, "manifest.json"), installer = releaseAsset(release, "install-mellow.sh");
    // Source checkpoints and incomplete asset uploads never enable binary
    // controls. Asset discovery supplies no GPU, Metal or WindowServer proof.
    if (zip && checksum && manifest && installer) return { release, zip, checksum, manifest, installer };
  }
  return null;
}
function shellQuote(value) { return "'" + value.replaceAll("'", "'\\''") + "'"; }
function renderRelease() {
  const available = releaseState === "available" && releasePackage;
  for (const id of ["binary-download", "checksum-link", "manifest-link"]) $("#" + id).hidden = !available;
  $("#copy-install").disabled = $("#copy-run").disabled = !available;
  if (available) {
    const { release, zip, checksum, manifest, installer } = releasePackage;
    $("#binary-download").href = safeReleaseUrl(zip.browser_download_url); $("#checksum-link").href = safeReleaseUrl(checksum.browser_download_url);
    $("#manifest-link").href = safeReleaseUrl(manifest.browser_download_url); $("#installer-source").href = safeReleaseUrl(installer.browser_download_url);
    $("#install-docs").href = `${REPO}/blob/${encodeURIComponent(release.tag_name)}/docs/INSTALLATION.md`;
    $("#release-info").textContent = `${release.tag_name} · ${language === "ko" ? "실험용 바이너리" : "experimental binaries"}`;
    $("#release-size").textContent = `${(zip.size / 1048576).toFixed(1)} MB · ZIP`;
    const download = `curl --fail --location --proto '=https' --tlsv1.2 --output install-mellow.sh ${shellQuote(safeReleaseUrl(installer.browser_download_url))}`;
    $("#install-code").textContent = download + "\nless ./install-mellow.sh";
    $("#run-code").textContent = `bash ./install-mellow.sh --version ${shellQuote(release.tag_name)}`;
    $("#install-status").textContent = language === "ko" ? "선택한 릴리스의 외부 ZIP 체크섬과 내부 payload 체크섬을 검사합니다. GPU 실행·보안 설정 변경·재부팅은 자동 실행하지 않습니다." : "The installer checks the release ZIP digest and internal payload checksums. It does not automatically run GPU workloads, change security settings or reboot.";
  } else {
    $("#release-info").textContent = releaseState === "loading" ? t("checkingReleases") : releaseState === "error" ?
      (language === "ko" ? "릴리스를 확인하지 못했습니다. GitHub에서 바이너리 자산을 확인해 주세요." : "Release discovery failed. Check binary assets on GitHub.") :
      (language === "ko" ? "설치용 바이너리가 아직 게시되지 않았습니다. 개발 소스는 Releases에서 확인하세요." : "No installable binary package is published yet. Development source is available in Releases.");
    $("#release-size").textContent = "";
    $("#install-code").textContent = language === "ko" ? "# 바이너리 릴리스가 확인되면 다운로드 명령이 표시됩니다." : "# Download commands appear once a complete binary release is available.";
    $("#run-code").textContent = language === "ko" ? "# 설치용 바이너리 릴리스 미확인" : "# No installable binary release confirmed";
    $("#install-status").textContent = t("installWaiting");
  }
}
async function copyCode(button, code) {
  try {
    await navigator.clipboard.writeText(code.textContent); button.textContent = language === "ko" ? "복사 완료" : "Copied";
    setTimeout(() => { button.textContent = t("copyCommands"); }, 1800);
  } catch {
    const selection = window.getSelection(), range = document.createRange(); range.selectNodeContents(code); selection.removeAllRanges(); selection.addRange(range);
    button.textContent = language === "ko" ? "선택한 텍스트를 복사하세요" : "Copy the selected text";
  }
}
$("#language").addEventListener("click", () => {
  language = language === "ko" ? "en" : "ko"; const url = new URL(location.href);
  if (language === "en") url.searchParams.set("lang", "en"); else url.searchParams.delete("lang");
  history.replaceState(null, "", url); applyLanguage();
});
$("#search").addEventListener("input", renderDevices);
for (const id of ["family", "stage"]) $("#" + id).addEventListener("change", renderDevices);
document.querySelectorAll("[data-vendor]").forEach((button) => button.addEventListener("click", () => {
  vendor = button.dataset.vendor; document.querySelectorAll("[data-vendor]").forEach((item) => item.setAttribute("aria-pressed", String(item.dataset.vendor === vendor)));
  if (data) { updateSelects(); renderDevices(); }
}));
$("#additional").addEventListener("change", () => {
  if (!$("#additional").checked && vendor === "AMD") {
    vendor = "all"; document.querySelectorAll("[data-vendor]").forEach((item) => item.setAttribute("aria-pressed", String(item.dataset.vendor === vendor)));
  }
  if (data) { updateSelects(); renderDevices(); }
});
$("#reset-filters").addEventListener("click", () => {
  $("#search").value = ""; $("#family").value = $("#stage").value = "all"; vendor = "all";
  document.querySelectorAll("[data-vendor]").forEach((item) => item.setAttribute("aria-pressed", String(item.dataset.vendor === vendor)));
  if (data) { updateSelects(); renderDevices(); } $("#search").focus();
});
$("#copy-install").addEventListener("click", () => copyCode($("#copy-install"), $("#install-code")));
$("#copy-run").addEventListener("click", () => copyCode($("#copy-run"), $("#run-code")));
applyLanguage();
fetchJson(new URL("./compatibility.json", import.meta.url)).then((value) => {
  data = validateData(value); deviceState = "available";
  $("#source-commit").textContent = data.source_ref.slice(0, 7); $("#source-commit").href = `${REPO}/commit/${data.source_ref}`;
  $("#checkpoint-link").href = repoFile("validation/native-gpu/direct-dma-checkpoint.json");
  document.querySelectorAll("[data-repo-path]").forEach((link) => { link.href = repoFile(link.dataset.repoPath); });
  updateSelects(); renderDevices();
}).catch(() => { deviceState = "error"; $("#data-error").hidden = false; $("#result-count").textContent = t("dataError"); });
fetchJson(RELEASES_API).then((releases) => {
  releasePackage = findBinaryPackage(releases); releaseState = releasePackage ? "available" : "missing"; renderRelease();
}).catch(() => { releaseState = "error"; renderRelease(); });
