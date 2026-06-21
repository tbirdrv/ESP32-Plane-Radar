"use strict";

const $ = (id) => document.getElementById(id);
const statusEl = $("status");
const kmPerMile = 1.609344;

function setStatus(msg, ok) {
  statusEl.textContent = msg || "";
  statusEl.className = "status" + (ok === false ? " err" : ok === true ? " ok" : "");
}

function currentModeFromFlags(clockOnly, radarOnly) {
  if (radarOnly) return "radar";
  if (clockOnly) return "clock";
  return "auto";
}

function applyMode(mode) {
  $("modeAuto").checked = mode === "auto";
  $("modeClock").checked = mode === "clock";
  $("modeRadar").checked = mode === "radar";
  const clockWindow = $("clockWindowSec");
  const clockWindowLabel = document.querySelector('label[for="clockWindowSec"]');
  clockWindow.disabled = mode !== "auto";
  if (clockWindowLabel) {
    clockWindowLabel.classList.toggle("disabled", mode !== "auto");
  }
}

function updateRangeOptionLabels(preferMiles) {
  const rangeSelect = $("rangeIndex");
  if (!rangeSelect) return;

  for (const option of rangeSelect.options) {
    const km = Number(option.dataset.km);
    if (!Number.isFinite(km)) continue;
    const miles = km / kmPerMile;
    const milesLabel = miles >= 10 ? miles.toFixed(1) : miles.toFixed(2);
    option.textContent = preferMiles
      ? `${milesLabel} mi (${km} km)`
      : `${km} km (${milesLabel} mi)`;
  }
}

async function load() {
  try {
    const r = await fetch("/api/radar");
    if (!r.ok) throw new Error("HTTP " + r.status);
    const d = await r.json();
    $("rlat").value = Number(d.lat).toFixed(6);
    $("rlon").value = Number(d.lon).toFixed(6);
    $("elevFt").value = Number(d.elevFt);
    $("rangeIndex").value = String(Number.isFinite(Number(d.rangeIndex)) ? Number(d.rangeIndex) : 1);
    $("useMiles").checked = !!d.useMiles;
    updateRangeOptionLabels($("useMiles").checked);
    $("showRunways").checked = !!d.showRunways;
    $("clockWindowSec").value = Number(d.clockWindowSec || 0);
    applyMode(currentModeFromFlags(!!d.clockOnly, !!d.radarOnly));
  } catch (e) {
    setStatus("Failed to load settings: " + e.message, false);
  }
}

async function save(ev) {
  ev.preventDefault();
  const mode = $("modeClock").checked ? "clock" : ($("modeRadar").checked ? "radar" : "auto");
  const body = {
    lat: parseFloat($("rlat").value),
    lon: parseFloat($("rlon").value),
    elevFt: parseInt($("elevFt").value, 10),
    rangeIndex: parseInt($("rangeIndex").value, 10),
    useMiles: $("useMiles").checked,
    showRunways: $("showRunways").checked,
    clockWindowSec: parseInt($("clockWindowSec").value, 10),
    clockOnly: mode === "clock",
    radarOnly: mode === "radar"
  };
  setStatus("Saving...");
  try {
    const r = await fetch("/api/radar", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body)
    });
    if (!r.ok) throw new Error("HTTP " + r.status);
    setStatus("Saved.", true);
    load();
  } catch (e) {
    setStatus("Save failed: " + e.message, false);
  }
}

$("modeAuto").addEventListener("change", () => applyMode("auto"));
$("modeClock").addEventListener("change", () => applyMode("clock"));
$("modeRadar").addEventListener("change", () => applyMode("radar"));
$("useMiles").addEventListener("change", () => updateRangeOptionLabels($("useMiles").checked));
$("radar-form").addEventListener("submit", save);
load();
