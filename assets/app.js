/* Copyright (c) 2026 Mellow contributors. SPDX-License-Identifier: MIT */
"use strict";
(() => {
  const REPO = "NiSeullent/Mellow";
  const GITHUB = "https://github.com/" + REPO;
  const API = "https://api.github.com/repos/" + REPO + "/releases?per_page=30";
  const statuses = {partial: "부분 구현", source_intake: "소스 검토", not_implemented: "미구현"};
  const state = {catalog: null, models: null, mode: "family", visibleLimit: 24, releases: [], command: "", runCommand: ""};
  const $ = id => document.getElementById(id);
  const node = (tag, className, value) => {
    const result = document.createElement(tag);
    if (className) result.className = className;
    if (value !== undefined) result.textContent = value;
    return result;
  };
  const validText = (value, max = 2000) => typeof value === "string" && value.length > 0 && value.length <= max;
  const texts = (value, max = 100) => Array.isArray(value) && value.length <= max && value.every(item => validText(item));
  const falseClaims = value => value && value.allModelsVerified === false &&
    value.nativeMetalVerified === false && value.systemWindowServerVerified === false;
  const validOs = value => Array.isArray(value) && value.length > 0 && value.length <= 2 &&
    value.every(item => item === 15 || item === 26) && new Set(value).size === value.length;
  const safeUrl = value => {
    try { const url = new URL(value); return url.protocol === "https:" && !url.username && !url.password ? url.href : null; }
    catch { return null; }
  };
  const githubUrl = (value, kind) => {
    const safe = safeUrl(value);
    if (!safe) return null;
    const url = new URL(safe);
    const prefix = "/" + REPO + "/releases/" + (kind === "asset" ? "download/" : "tag/");
    return url.origin === "https://github.com" && url.pathname.startsWith(prefix) && !url.search && !url.hash ? url.href : null;
  };
  function link(label, href, className) {
    const a = node("a", className, label);
    a.href = href; a.target = "_blank"; a.rel = "noopener noreferrer";
    return a;
  }
  function showState(parent, message, retry) {
    parent.replaceChildren();
    const panel = node("div", "state-panel", message);
    if (retry) { const button = node("button", "", "다시 확인"); button.type = "button"; button.addEventListener("click", retry); panel.append(button); }
    parent.append(panel); parent.setAttribute("aria-busy", "false");
  }
  async function readJson(url, maxBytes) {
    const controller = new AbortController();
    const timeout = window.setTimeout(() => controller.abort(), 15000);
    try {
      const response = await fetch(url, {signal: controller.signal, credentials: "omit", headers: {Accept: "application/json"}});
      if (!response.ok) throw new Error("HTTP " + response.status);
      const raw = await response.text();
      if (new TextEncoder().encode(raw).length > maxBytes) throw new Error("Oversized metadata");
      return JSON.parse(raw);
    } finally { window.clearTimeout(timeout); }
  }
  function validateCatalog(value) {
    if (!value || value.schemaVersion !== 1 || value.catalogScope !== "source-families" ||
        !validText(value.sourceRevision, 64) || !validText(value.generatedAt, 80) ||
        !falseClaims(value.claims) || !Array.isArray(value.devices) || value.devices.length > 500)
      throw new Error("Invalid family catalog");
    const ids = new Set();
    for (const d of value.devices) {
      if (!d || !validText(d.id, 100) || ids.has(d.id) || !["Intel", "NVIDIA"].includes(d.vendor) ||
          !validText(d.family, 160) || !texts(d.architectures) || !texts(d.modelExamples) ||
          !Array.isArray(d.pciDeviceIds) || d.pciDeviceIds.length !== 0 || !validOs(d.osTargets) ||
          !Object.hasOwn(statuses, d.implementationStatus) || d.validationStatus !== "not_run" ||
          !validText(d.summary) || !texts(d.blockers) || !texts(d.sourcePaths) || !texts(d.sourceLinks) ||
          !d.sourceLinks.every(value => safeUrl(value))) throw new Error("Invalid family entry");
      ids.add(d.id);
    }
    return value;
  }
  function validateModels(value) {
    if (!value || value.schemaVersion !== 1 || value.catalogScope !== "declared-models" ||
        !validText(value.sourceRevision, 64) || !falseClaims(value.claims) ||
        !value.coverage || value.coverage.exhaustive !== false || !texts(value.coverage.notes) ||
        !Array.isArray(value.models) || value.models.length > 10000) throw new Error("Invalid model catalog");
    const ids = new Set(), families = new Map(state.catalog.devices.map(d => [d.id, d]));
    for (const d of value.models) {
      if (!d || !validText(d.id, 200) || ids.has(d.id) || (d.familyId !== null && !families.has(d.familyId)) ||
          !validText(d.name, 250) || !["Intel", "NVIDIA"].includes(d.vendor) ||
          (d.familyId !== null && families.get(d.familyId).vendor !== d.vendor) ||
          !Array.isArray(d.declaredIds) || d.declaredIds.length > 500 ||
          !d.declaredIds.every(id => typeof id === "string" && /^(8086|10DE):[0-9A-F]{4}$/.test(id) && id.startsWith(d.vendor === "Intel" ? "8086:" : "10DE:")) ||
          !Array.isArray(d.measuredIds) || d.measuredIds.length !== 0 || !validOs(d.osTargets) ||
          !Object.hasOwn(statuses, d.implementationStatus) || d.validationStatus !== "not_run" ||
          !texts(d.sourceLinks) || !d.sourceLinks.every(value => safeUrl(value)) || !texts(d.blockers))
        throw new Error("Invalid declared-model entry");
      if (d.unqualifiedDeclaredIds !== undefined && (!Array.isArray(d.unqualifiedDeclaredIds) ||
          !d.unqualifiedDeclaredIds.every(id => d.declaredIds.includes(id)))) throw new Error("Invalid unqualified ID declaration");
      if (d.declaredSubsystemIds !== undefined && (!Array.isArray(d.declaredSubsystemIds) || d.declaredSubsystemIds.length > 1000 ||
          !d.declaredSubsystemIds.every(id => id && [id.vendorId, id.deviceId, id.subsystemVendorId, id.subsystemDeviceId].every(part =>
            typeof part === "string" && /^[0-9A-F]{4}$/.test(part)) && d.declaredIds.includes(id.vendorId + ":" + id.deviceId))))
        throw new Error("Invalid OEM/subsystem declaration");
      ids.add(d.id);
    }
    return value;
  }
  function card(d) {
    const model = state.mode === "model";
    const family = model ? state.catalog.devices.find(item => item.id === d.familyId) : d;
    const article = node("article", "gpu-card");
    const top = node("div", "gpu-card-top");
    top.append(node("span", "vendor-label", d.vendor), node("span", "status-badge status-" + d.implementationStatus, statuses[d.implementationStatus]));
    article.append(top, node("h3", "", model ? d.name : d.family));
    article.append(node("p", "architecture", model ? (family ? family.family : "공식 소스 선언 · 계열 미분류") : d.architectures.join(" · ")));
    if (!model && d.modelExamples.length) article.append(node("p", "models", d.modelExamples.join(" · ")));
    if (!model) article.append(node("p", "gpu-summary", d.summary));
    const facts = node("div", "gpu-facts");
    for (const os of d.osTargets) facts.append(node("span", "fact-chip", "macOS " + os + " 대상"));
    facts.append(node("span", "fact-chip validation", "실기 검증 전"));
    article.append(facts, node("p", "pci-note", model && d.declaredIds.length ?
      "소스 선언 ID: " + d.declaredIds.join(", ") + " · 실기 호환성 미확인" :
      "개별 PCI ID 실기 호환성 미확인"));
    if (model && d.declaredSubsystemIds && d.declaredSubsystemIds.length) article.append(node("p", "pci-note",
      "OEM·subsystem 조건이 있는 선언 " + d.declaredSubsystemIds.length + "건 · PCI ID만으로 모델 확정 불가"));
    const details = node("details", "card-details");
    details.append(node("summary", "", "남은 구현 · 출처"));
    const list = node("ul");
    for (const blocker of d.blockers) list.append(node("li", "", blocker));
    if (model) list.append(node("li", "", "공식 소스의 ID 선언은 macOS 실행 지원이나 실기 검증 결과가 아닙니다."));
    if (model && d.declaredSubsystemIds) for (const id of d.declaredSubsystemIds.slice(0, 20)) list.append(node("li", "",
      "소스 OEM 선언: " + id.vendorId + ":" + id.deviceId + " / " + id.subsystemVendorId + ":" + id.subsystemDeviceId));
    if (model && d.declaredSubsystemIds && d.declaredSubsystemIds.length > 20) list.append(node("li", "", "나머지 OEM 선언은 모델 데이터 원본에서 확인하세요."));
    details.append(list);
    d.sourceLinks.slice(0, 4).forEach((url, index) => details.append(link(index ? "참고 자료 " + index + " ↗" : "개발 · 출처 ↗", safeUrl(url))));
    article.append(details); return article;
  }
  function renderCatalog() {
    if (!state.catalog) return;
    const terms = $("gpu-search").value.trim().toLocaleLowerCase().replace(/0x(?=[0-9a-f]{4})/g, "").split(/\s+/).filter(Boolean);
    const vendor = $("vendor-filter").value, status = $("status-filter").value, os = $("os-filter").value;
    const source = state.mode === "model" && state.models ? state.models.models : state.catalog.devices;
    const visible = source.filter(d => {
      const words = state.mode === "model" ? [d.name, d.vendor, d.familyId, ...d.declaredIds,
        ...(d.declaredSubsystemIds || []).map(id => id.vendorId + ":" + id.deviceId + "/" + id.subsystemVendorId + ":" + id.subsystemDeviceId)] :
        [d.family, d.vendor, d.id, ...d.architectures, ...d.modelExamples];
      const haystack = words.join(" ").toLocaleLowerCase();
      return (vendor === "all" || d.vendor === vendor) && (status === "all" || d.implementationStatus === status) &&
        (os === "all" || d.osTargets.includes(Number(os))) && terms.every(term => haystack.includes(term));
    });
    const shown = visible.slice(0, state.visibleLimit);
    $("catalog-count").textContent = "총 " + source.length + (state.mode === "model" ? "개 소스 선언 모델 · " : "개 계열 · ") + visible.length + "개 일치 · " + shown.length + "개 표시";
    const grid = $("catalog-grid"); grid.replaceChildren(); grid.setAttribute("aria-busy", "false");
    if (!visible.length) showState(grid, "검색 결과가 없습니다. 목록에 없거나 실기 검증이 없는 GPU의 호환성은 미확인입니다.");
    else { const fragment = document.createDocumentFragment(); for (const d of shown) fragment.append(card(d)); grid.append(fragment); }
    $("catalog-more").hidden = shown.length >= visible.length;
    $("catalog-more").textContent = Math.min(24, visible.length - shown.length) + "개 더 보기";
    $("family-mode").setAttribute("aria-pressed", String(state.mode === "family"));
    $("model-mode").setAttribute("aria-pressed", String(state.mode === "model"));
    const metadata = state.mode === "model" ? state.models : state.catalog;
    $("catalog-source").replaceChildren(node("span", "", "기준 소스 " + metadata.sourceRevision.slice(0, 12) + " · "));
    $("catalog-source").append(link(state.mode === "model" ? "모델 데이터 원본" : "계열 데이터 원본", new URL(state.mode === "model" ? "./data/models.json" : "./data/compatibility.json", document.baseURI).href));
    if (state.mode === "model") $("catalog-source").append(node("span", "", " · 전체 모델을 빠짐없이 망라하는 목록이 아닙니다."));
  }
  async function loadCatalog() {
    $("catalog-grid").setAttribute("aria-busy", "true");
    try {
      state.catalog = validateCatalog(await readJson("./data/compatibility.json", 1500000));
      renderCatalog();
      try {
        state.models = validateModels(await readJson("./data/models.json", 8000000));
        $("model-mode").disabled = false;
      } catch { $("model-mode").disabled = true; $("model-mode").title = "모델 선언 목록이 게시되지 않았거나 검증되지 않았습니다."; }
    } catch {
      $("catalog-count").textContent = "개발 현황을 불러오지 못했습니다.";
      showState($("catalog-grid"), "현재 계열 정보를 확인할 수 없습니다. 아래 개발 문서 또는 데이터 원본을 확인하세요.", loadCatalog);
    }
  }
  const PACKAGE_ZIP = "Mellow-macos-x86_64.zip";
  const PACKAGE_REQUIRED = [PACKAGE_ZIP, PACKAGE_ZIP + ".sha256", "manifest.json", "install-mellow.sh", "uninstall-mellow.sh"];
  const PACKAGE_ASSETS = new Set(PACKAGE_REQUIRED);
  const shellQuote = value => "'" + value.replace(/'/g, "'\\''") + "'";
  function completePackage(release) {
    if (!validText(release.tag_name, 200) || /[\s\x00-\x1f\x7f]/.test(release.tag_name) ||
        !Number.isFinite(Date.parse(release.published_at))) return null;
    const result = {};
    for (const name of PACKAGE_REQUIRED) {
      const matches = release.assets.filter(asset => asset.name === name);
      if (matches.length !== 1) return null;
      const asset = matches[0], safe = githubUrl(asset.browser_download_url, "asset");
      if (!safe || asset.state !== "uploaded" || !Number.isSafeInteger(asset.size) || asset.size <= 0) return null;
      // Bind every required asset to this release tag and exact filename.
      // Merely sharing the official repository prefix is insufficient.
      const parts = new URL(safe).pathname.slice(("/" + REPO + "/releases/download/").length).split("/");
      try {
        if (parts.length !== 2 || decodeURIComponent(parts[0]) !== release.tag_name || decodeURIComponent(parts[1]) !== name) return null;
      } catch { return null; }
      result[name] = asset;
    }
    return result;
  }
  const hasPackageAssets = release => release.assets.some(asset => PACKAGE_ASSETS.has(asset.name) && asset.name !== "manifest.json");
  const assetKind = name => {
    if (name === PACKAGE_ZIP) return "coherent_bundle";
    if (name === "install-mellow.sh") return "installer";
    if (name === "mellow-install.sh" || /^mellow-installer-macos(15|26)-x86_64$/.test(name)) return "installer";
    if (/^mellow-development-macos(15|26)-x86_64\.tar$/.test(name)) return "bundle";
    if (/source/i.test(name) && /\.(zip|tar|gz)$/i.test(name)) return "source";
    if (/native-development|kext/i.test(name) && /\.(zip|tar|gz)$/i.test(name)) return "kernel";
    if (/framework|userspace/i.test(name) && /\.(zip|tar|gz)$/i.test(name)) return "framework";
    return "other";
  };
  function size(bytes) { return bytes >= 1048576 ? (bytes / 1048576).toFixed(1) + " MB" : (bytes / 1024).toFixed(1) + " KB"; }
  function downloadCard(asset) {
    const kind = assetKind(asset.name);
    const labels = {
      installer: ["CLI INSTALLER", "터미널 설치 도구", "버전별 사용자 설치를 위한 도구입니다. 설치만으로 GPU 가속이나 드라이버 활성화가 검증되지는 않습니다."],
      bundle: ["DEVELOPMENT BUNDLE", /macos26/.test(asset.name) ? "macOS 26 개발 패키지" : "macOS 15 개발 패키지", "실제 kext·앱용 프레임워크·진단 도구 묶음입니다. GPU 실기 지원과 시스템 Metal 등록은 별도 검증이 필요합니다."],
      coherent_bundle: ["COMPILED DEVELOPMENT PACKAGE", "macOS 15 / 26 · Intel x86_64", "Mellow.kext·앱용 framework·CLI의 실험용 바이너리입니다. Developer ID 서명·공증과 native GPU·시스템 Metal·WindowServer 실기 검증은 완료되지 않았습니다."],
      source: ["SOURCE CODE", "소스 코드", "해당 릴리스의 구현, 문서와 고지 사항을 직접 확인할 수 있습니다."],
      kernel: ["NATIVE DEVELOPMENT", "개발용 커널 패키지", "게시된 네이티브 개발 자산입니다. kext 빌드와 실제 적재·GPU 가속은 서로 다른 검증입니다."],
      framework: ["APP FRAMEWORK", "앱용 그래픽 프레임워크", "기존 호스트 GPU를 사용하는 명시적 앱 어댑터입니다. 시스템 Metal 드라이버가 아닙니다."]
    };
    const content = labels[kind], card = node("article", "download-card" + (["bundle", "coherent_bundle"].includes(kind) ? " featured" : ""));
    card.append(node("p", "download-kind", content[0]), node("h3", "", content[1]), node("p", "", content[2]));
    card.append(node("div", "asset-name", asset.name + " · " + size(asset.size)));
    const a = link("다운로드", asset.browser_download_url, "download-button"); a.append(node("span", "", "↓")); card.append(a);
    return card;
  }
  async function copyCommand(button, command) {
    if (!command) return;
    try { await navigator.clipboard.writeText(command); button.textContent = "복사됨"; }
    catch { button.textContent = "직접 선택"; }
    window.setTimeout(() => { button.textContent = "복사"; }, 2000);
  }
  function packageRunTerminal() {
    if ($("package-run-terminal")) return;
    const review = node("p", "source-caption", "첫 번째 명령으로 스크립트를 내려받아 읽고, 확인한 뒤 두 번째 설치 명령을 별도로 실행하세요.");
    review.id = "package-review"; review.hidden = true;
    const terminal = node("div", "terminal"), bar = node("div", "terminal-bar"), button = node("button", "", "복사");
    terminal.id = "package-run-terminal"; terminal.hidden = true;
    button.id = "copy-package-run"; button.type = "button"; button.disabled = true;
    button.setAttribute("aria-label", "선택한 버전 설치 명령 복사");
    button.addEventListener("click", () => copyCommand(button, state.runCommand));
    bar.append(node("span", "", "선택한 버전 설치"), button);
    const pre = node("pre"), code = node("code"); code.id = "package-run-command"; pre.append(code);
    terminal.append(bar, pre); $("installer-status").before(review, terminal);
  }
  function installer(release) {
    packageRunTerminal();
    const bundle = completePackage(release);
    state.runCommand = ""; $("copy-package-run").disabled = true;
    $("package-review").hidden = $("package-run-terminal").hidden = !bundle;
    $("package-run-command").textContent = "";
    if (bundle) {
      state.command = "curl --fail --location --proto '=https' --proto-redir '=https' --tlsv1.2 --output install-mellow.sh " +
        shellQuote(bundle["install-mellow.sh"].browser_download_url) + " &&\nless ./install-mellow.sh";
      state.runCommand = "bash ./install-mellow.sh --version " + shellQuote(release.tag_name);
      $("install-command").textContent = state.command; $("copy-install").disabled = false;
      $("package-run-command").textContent = state.runCommand; $("copy-package-run").disabled = false;
      $("installer-status").replaceChildren(node("span", "", "기본 설치 위치는 ~/Library/Mellow입니다. 외부 ZIP·내부 payload 체크섬을 검사하며, GPU 실행·시스템 kext 설치·보안 변경·재부팅은 자동 실행하지 않습니다. "));
      $("installer-status").append(link("선택한 버전 설치 문서 ↗", GITHUB + "/blob/" + encodeURIComponent(release.tag_name) + "/docs/INSTALLATION.md"));
      return;
    }
    const asset = release.assets.find(item => item.name === "mellow-install.sh");
    const installable = asset && /^sha256:[0-9a-f]{64}$/.test(asset.digest || "") &&
      /^[A-Za-z0-9][A-Za-z0-9._-]{0,95}$/.test(release.tag_name) &&
      [15, 26].every(major => release.assets.some(item => item.name === "mellow-installer-macos" + major + "-x86_64") &&
        release.assets.some(item => item.name === "mellow-development-macos" + major + "-x86_64.tar"));
    state.command = ""; $("copy-install").disabled = true;
    if (installable) {
      // Only an actual API-returned asset URL is inserted. No shell execution.
      state.command = "curl -fL --proto '=https' --proto-redir '=https' --output mellow-install.sh " + shellQuote(asset.browser_download_url) + " &&\n" +
        "printf '%s  %s\\n' '" + asset.digest.slice(7) + "' 'mellow-install.sh' | shasum -a 256 -c - &&\n" +
        "bash ./mellow-install.sh --help &&\n" +
        "bash ./mellow-install.sh --release " + shellQuote(release.tag_name);
      $("install-command").textContent = state.command;
      $("copy-install").disabled = false;
      $("installer-status").textContent = "공식 GitHub 자산의 SHA256을 확인한 뒤 도움말과 사용자 폴더 설치를 실행하는 명령입니다. kext 활성화와 시스템 Metal 등록은 수행하지 않습니다.";
    } else {
      $("install-command").textContent = hasPackageAssets(release) ? "# 새 패키지의 ZIP·체크섬·manifest·설치/제거 도구 업로드가 아직 완성되지 않았습니다." : "# 선택한 릴리스의 설치 도구·두 OS 패키지·SHA256을\n# 모두 확인할 수 없습니다. 게시된 파일은 위에서 확인하세요.";
      $("installer-status").textContent = "설치에 필요한 실제 자산과 체크섬이 모두 게시된 릴리스에서 설치 명령을 제공합니다.";
    }
  }
  function renderRelease(index) {
    const release = state.releases[index]; if (!release) return;
    $("release-page").href = release.html_url;
    const date = new Date(release.published_at);
    $("release-status").textContent = (release.prerelease ? "개발 프리릴리스" : "게시된 릴리스") + " · " +
      (Number.isNaN(date.valueOf()) ? release.tag_name : date.toLocaleDateString("ko-KR")) + " · 실제 자산 " + release.assets.length + "개";
    const grid = $("download-grid"); grid.replaceChildren(); grid.setAttribute("aria-busy", "false");
    const bundle = completePackage(release), pendingPackage = hasPackageAssets(release) && !bundle;
    const visibleAssets = release.assets.filter(a => !pendingPackage || !PACKAGE_ASSETS.has(a.name));
    const primary = visibleAssets.filter(a => assetKind(a.name) !== "other");
    if (primary.length) for (const a of primary) grid.append(downloadCard(a));
    else showState(grid, pendingPackage ? "새 바이너리 패키지 업로드가 아직 완성되지 않았습니다. ZIP·체크섬·manifest·설치/제거 도구가 모두 게시되면 다운로드가 활성화됩니다." : "이 릴리스에는 분류 가능한 kext·프레임워크·설치 도구·소스 패키지가 없습니다. 실제 자산과 릴리스 설명을 확인하세요.");
    const other = $("other-assets"); other.replaceChildren();
    for (const asset of visibleAssets.filter(a => assetKind(a.name) === "other")) {
      const a = link("", asset.browser_download_url); a.append(node("span", "asset-label", asset.name), node("span", "asset-size", size(asset.size) + " ↓")); other.append(a);
    }
    if (!other.children.length) other.append(node("p", "", "별도의 기타 자산이 없습니다."));
    installer(release);
  }
  async function loadReleases() {
    $("download-grid").setAttribute("aria-busy", "true");
    try {
      const raw = await readJson(API, 4000000);
      if (!Array.isArray(raw) || raw.length > 30) throw new Error("Invalid release list");
      state.releases = raw.filter(r => r && !r.draft && validText(r.tag_name, 200) && githubUrl(r.html_url, "release") && Array.isArray(r.assets))
        .map(r => ({tag_name: r.tag_name, name: validText(r.name, 400) ? r.name : r.tag_name, html_url: githubUrl(r.html_url, "release"),
          prerelease: r.prerelease === true, published_at: r.published_at, assets: r.assets.filter(a => a && validText(a.name, 300) &&
            Number.isSafeInteger(a.size) && a.size >= 0 && githubUrl(a.browser_download_url, "asset")).map(a => ({
              name: a.name, size: a.size, state: a.state, digest: /^sha256:[0-9a-f]{64}$/.test(a.digest || "") ? a.digest : null,
              browser_download_url: githubUrl(a.browser_download_url, "asset")}))}));
      if (!state.releases.length) throw new Error("No published releases");
      const select = $("release-select"); select.replaceChildren();
      state.releases.forEach((r, i) => { const o = node("option", "", r.name + (r.prerelease ? " · 개발" : "")); o.value = String(i); select.append(o); });
      const newestPackage = state.releases.reduce((best, release, index) => completePackage(release) &&
        (best < 0 || Date.parse(release.published_at) > Date.parse(state.releases[best].published_at)) ? index : best, -1);
      const preferred = newestPackage >= 0 ? newestPackage : state.releases.findIndex(r => r.assets.some(a => ["bundle", "kernel", "framework"].includes(assetKind(a.name))));
      select.value = String(preferred >= 0 ? preferred : 0); select.disabled = false; renderRelease(Number(select.value));
    } catch {
      $("release-status").textContent = "GitHub API에 연결하지 못했습니다. 네트워크나 API 요청 제한을 확인하세요.";
      $("release-select").disabled = true;
      showState($("download-grid"), "다운로드 파일을 확인할 수 없습니다. '모든 릴리스'에서 GitHub의 실제 게시 파일을 확인하세요.", loadReleases);
      $("other-assets").replaceChildren(link("GitHub 릴리스에서 확인 ↗", GITHUB + "/releases"));
      state.command = state.runCommand = ""; $("copy-install").disabled = true;
      $("install-command").textContent = "# 공개 릴리스 자산을 확인하지 못했습니다.";
      $("installer-status").textContent = "공개 릴리스 자산을 확인할 수 없어 설치 명령을 비활성화했습니다. GitHub의 릴리스 설명과 실제 파일을 확인하세요.";
      if ($("package-run-terminal")) {
        $("package-review").hidden = $("package-run-terminal").hidden = true; $("copy-package-run").disabled = true;
      }
    }
  }
  $("catalog-filters").addEventListener("submit", event => event.preventDefault());
  ["gpu-search", "vendor-filter", "status-filter", "os-filter"].forEach(id => $(id).addEventListener(id === "gpu-search" ? "input" : "change", () => { state.visibleLimit = 24; renderCatalog(); }));
  $("family-mode").addEventListener("click", () => { state.mode = "family"; state.visibleLimit = 24; renderCatalog(); });
  $("model-mode").addEventListener("click", () => { if (state.models) { state.mode = "model"; state.visibleLimit = 24; renderCatalog(); } });
  $("catalog-more").addEventListener("click", () => { state.visibleLimit += 24; renderCatalog(); });
  $("release-select").addEventListener("change", () => renderRelease(Number($("release-select").value)));
  $("copy-install").addEventListener("click", () => copyCommand($("copy-install"), state.command));
  loadCatalog(); loadReleases();
})();
