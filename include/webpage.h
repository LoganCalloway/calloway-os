#pragma once

// =============================================================================
// WEB DASHBOARD PAGE — HTML/CSS/JS served at omnicore.local
// Split out of main.cpp purely for readability; #include'd directly into
// main.cpp's translation unit (not a separate .cpp), so it changes nothing
// about how the firmware compiles or links.
//
// Page lives in flash, not RAM. JS polls /data every 2s and patches the DOM,
// so the page never reloads and never flickers. Graphs load /history once on
// page open (24hr backlog) then append one new point every 60s from /data —
// no need to refetch the whole history each time.
// =============================================================================
static const char PAGE_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Omni-Core</title>
<script src='https://cdnjs.cloudflare.com/ajax/libs/Chart.js/4.5.0/chart.umd.min.js'></script>
<style>
  body{background:#050505;color:#0ff;font-family:'Courier New',monospace;
       margin:0;padding:20px;}
  h1{color:#0f0;text-align:center;letter-spacing:3px;margin:0 0 4px;font-size:1.4em;}
  h2{color:#0ff;text-align:left;letter-spacing:2px;font-size:.85em;
     text-transform:uppercase;margin:28px 0 10px;max-width:1100px;
     margin-left:auto;margin-right:auto;border-bottom:1px solid #044;padding-bottom:6px;}
  .sub{text-align:center;color:#055;font-size:.75em;margin-bottom:24px;}
  .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));
        gap:12px;max-width:800px;margin:0 auto;}
  .card{background:#0a0a0a;border:1px solid #044;padding:14px;text-align:center;}
  .card:hover{border-color:#0ff;}
  .lbl{font-size:.65em;color:#077;text-transform:uppercase;letter-spacing:2px;}
  .val{font-size:1.7em;font-weight:bold;margin-top:6px;}
  .unit{font-size:.5em;color:#066;margin-left:3px;}
  .good{color:#0f0;} .warn{color:#ff0;} .bad{color:#f33;} .neut{color:#0ff;}
  .foot{text-align:center;color:#044;font-size:.65em;margin-top:24px;}
  .chartwrap{background:#0a0a0a;border:1px solid #044;padding:16px;
             max-width:1100px;margin:0 auto 20px;}
  .chartwrap canvas{max-height:220px;}
  .loading{text-align:center;color:#044;font-size:.75em;padding:20px;}
  .rangebar{max-width:1100px;margin:20px auto 4px;display:flex;
            align-items:center;gap:8px;}
  .rangelbl{font-size:.65em;color:#077;text-transform:uppercase;
            letter-spacing:2px;margin-right:4px;}
  .rangebtn{background:#0a0a0a;border:1px solid #044;color:#077;
            font-family:'Courier New',monospace;font-size:.7em;
            letter-spacing:1px;padding:6px 14px;cursor:pointer;
            text-transform:uppercase;}
  .rangebtn:hover{border-color:#0ff;color:#0ff;}
  .rangebtn.active{border-color:#0ff;color:#0ff;background:#001a1a;}
  .statusbar{text-align:center;margin:4px 0 20px;font-size:.85em;
             letter-spacing:2px;text-transform:uppercase;font-weight:bold;}
  .statusbar .dot{display:inline-block;width:9px;height:9px;border-radius:50%;
                  margin-right:8px;vertical-align:middle;}
  .statusbar.status-good{color:#0f0;} .statusbar.status-good .dot{background:#0f0;}
  .statusbar.status-warn{color:#ff0;} .statusbar.status-warn .dot{background:#ff0;}
  .statusbar.status-bad{color:#f33;}  .statusbar.status-bad .dot{background:#f33;}
</style></head><body>
<h1>OMNI-CORE</h1>
<div class='sub' id='city'>&nbsp;</div>
<div class='statusbar status-good' id='statusbar'><span class='dot'></span><span id='statustext'>AIR QUALITY: --</span></div>
<div class='grid'>
  <div class='card'><div class='lbl'>CO2</div>
    <div class='val neut' id='co2'>--<span class='unit'>ppm</span></div></div>
  <div class='card'><div class='lbl'>PM2.5</div>
    <div class='val neut' id='pm25'>--<span class='unit'>ug/m3</span></div></div>
  <div class='card'><div class='lbl'>VOC Index</div>
    <div class='val neut' id='voc'>--</div></div>
  <div class='card'><div class='lbl'>Temp</div>
    <div class='val neut' id='temp'>--<span class='unit'>F</span></div></div>
  <div class='card'><div class='lbl'>Humidity</div>
    <div class='val neut' id='hum'>--<span class='unit'>%</span></div></div>
  <div class='card'><div class='lbl'>Light</div>
    <div class='val neut' id='lux'>--<span class='unit'>lux</span></div></div>
  <div class='card'><div class='lbl'>PM1</div>
    <div class='val neut' id='pm1'>--</div></div>
  <div class='card'><div class='lbl'>PM4</div>
    <div class='val neut' id='pm4'>--</div></div>
  <div class='card'><div class='lbl'>PM10</div>
    <div class='val neut' id='pm10'>--</div></div>
</div>

<div class='rangebar'>
  <span class='rangelbl'>Range:</span>
  <button class='rangebtn' data-hours='1'>1H</button>
  <button class='rangebtn' data-hours='6'>6H</button>
  <button class='rangebtn' data-hours='12'>12H</button>
  <button class='rangebtn active' data-hours='24'>24H</button>
</div>

<h2 id='hdrCO2'>CO2 &middot; Last 24 Hours</h2>
<div class='chartwrap'><canvas id='chartCO2'></canvas></div>

<h2 id='hdrPM'>Particulate Matter (ug/m3) &middot; Last 24 Hours</h2>
<div class='chartwrap'><canvas id='chartPM'></canvas></div>

<h2 id='hdrVOC'>VOC Index &middot; Last 24 Hours</h2>
<div class='chartwrap'><canvas id='chartVOC'></canvas></div>

<h2 id='hdrTemp'>Temperature (F) &middot; Last 24 Hours</h2>
<div class='chartwrap'><canvas id='chartTemp'></canvas></div>

<h2 id='hdrHum'>Humidity (%) &middot; Last 24 Hours</h2>
<div class='chartwrap'><canvas id='chartHum'></canvas></div>

<div class='foot'>CALLOWAY OS &middot; live &middot; cards update every 2s &middot; graphs every 60s</div>
<script>
function cls(el,c){el.className='val '+c;}

function tick(){
  fetch('/data').then(r=>r.json()).then(d=>{
    let co2=document.getElementById('co2');
    co2.innerHTML=d.co2+"<span class='unit'>ppm</span>";
    cls(co2, d.co2<800?'good':d.co2<1200?'warn':'bad');

    let pm=document.getElementById('pm25');
    pm.innerHTML=d.pm25.toFixed(1)+"<span class='unit'>ug/m3</span>";
    cls(pm, d.pm25<12?'good':d.pm25<35?'warn':'bad');

    document.getElementById('voc').textContent  = d.voc;
    document.getElementById('pm1').textContent  = d.pm1.toFixed(1);
    document.getElementById('pm4').textContent  = d.pm4.toFixed(1);
    document.getElementById('pm10').textContent = d.pm10.toFixed(1);
    document.getElementById('temp').innerHTML   = d.temp.toFixed(1)+"<span class='unit'>F</span>";
    document.getElementById('hum').innerHTML    = d.hum.toFixed(0)+"<span class='unit'>%</span>";
    document.getElementById('lux').innerHTML    = d.lux.toFixed(0)+"<span class='unit'>lux</span>";
    document.getElementById('city').textContent = d.city;

    // Overall status banner — worst of CO2/PM2.5 bands, same thresholds as
    // the live cards and charts above. VOC has no widely-agreed thresholds
    // so it doesn't drive this verdict; temp/humidity are comfort metrics,
    // not air-quality/health ones, so they're left out too.
    const co2Band = d.co2 < 800 ? 0 : d.co2 < 1200 ? 1 : 2;
    const pmBand  = d.pm25 < 12 ? 0 : d.pm25 < 35 ? 1 : 2;
    const worst = Math.max(co2Band, pmBand);
    const statusbar = document.getElementById('statusbar');
    const statustext = document.getElementById('statustext');
    if (worst === 0) {
      statusbar.className = 'statusbar status-good';
      statustext.textContent = 'AIR QUALITY: GOOD';
    } else if (worst === 1) {
      statusbar.className = 'statusbar status-warn';
      statustext.textContent = 'AIR QUALITY: ELEVATED';
    } else {
      statusbar.className = 'statusbar status-bad';
      statustext.textContent = 'AIR QUALITY: POOR';
    }
  }).catch(e=>{});
}

// Live cards start immediately, independent of whether Chart.js loaded.
// If the CDN is unreachable this is the one thing that must keep working.
tick();
setInterval(tick, 2000);

// ---- Chart setup -----------------------------------------------------
// Everything below only runs if Chart.js actually loaded from the CDN.
// If the CDN was blocked or unreachable, the charts are skipped entirely
// and the boxes show a message instead — the live cards above are
// unaffected either way since they're wired up before this check runs.
if (typeof Chart === 'undefined') {
  document.querySelectorAll('.chartwrap').forEach(el=>{
    el.innerHTML = "<div class='loading'>Chart library failed to load — check network access or the CDN URL/version in webpage.h</div>";
  });
} else {

// Threshold bands mirror the same breakpoints used on the TFT and the
// live cards: CO2 good/warn/bad at 800/1200ppm, PM2.5 at 12/35 ug/m3.
Chart.defaults.color = '#077';
Chart.defaults.borderColor = '#044';
Chart.defaults.font.family = "'Courier New', monospace";

function fmtTime(epochSec){
  const d = new Date(epochSec * 1000);
  return d.toLocaleTimeString([], {hour:'2-digit', minute:'2-digit'});
}


// Colors a line by segment based on value thresholds, so the line itself
// shifts green/yellow/red as it crosses bands rather than one flat color.
function bandColor(value, goodMax, warnMax){
  if (value < goodMax) return '#0f0';
  if (value < warnMax) return '#ff0';
  return '#f33';
}
function segmentColorFn(chart, goodMax, warnMax){
  return (ctx) => {
    const v = ctx.p1?.parsed?.y ?? ctx.p0?.parsed?.y ?? 0;
    return bandColor(v, goodMax, warnMax);
  };
}

let chartCO2, chartPM, chartTemp, chartHum, chartVOC;

function buildCharts(labels, co2Data, pm1Data, pm25Data, pm4Data, pm10Data, tempData, humData, vocData){
  const commonScales = {
    x: { ticks: { maxTicksLimit: 8, color:'#077' }, grid:{ color:'#022' } },
    y: { ticks: { color:'#077' }, grid:{ color:'#022' } }
  };

  chartCO2 = new Chart(document.getElementById('chartCO2'), {
    type: 'line',
    data: { labels, datasets: [{
      label: 'CO2 (ppm)', data: co2Data,
      borderWidth: 2, pointRadius: 0, tension: 0.25,
      segment: { borderColor: ctx => segmentColorFn(chartCO2, 800, 1200)(ctx) }
    }]},
    options: {
      responsive: true, animation: false,
      plugins: { legend: { display:false } },
      scales: commonScales
    }
  });

  chartPM = new Chart(document.getElementById('chartPM'), {
    type: 'line',
    data: { labels, datasets: [
      { label:'PM1',   data: pm1Data,  borderColor:'#066', borderWidth:1, pointRadius:0, tension:0.25 },
      { label:'PM2.5', data: pm25Data, borderWidth:2, pointRadius:0, tension:0.25,
        segment:{ borderColor: ctx => segmentColorFn(chartPM, 12, 35)(ctx) } },
      { label:'PM4',   data: pm4Data,  borderColor:'#088', borderWidth:1, pointRadius:0, tension:0.25 },
      { label:'PM10',  data: pm10Data, borderColor:'#0aa', borderWidth:1, pointRadius:0, tension:0.25 }
    ]},
    options: {
      responsive: true, animation: false,
      plugins: { legend: { display:true, labels:{ boxWidth:12, font:{size:10} } } },
      scales: commonScales
    }
  });

  chartTemp = new Chart(document.getElementById('chartTemp'), {
    type: 'line',
    data: { labels, datasets: [{
      label: 'Temp (F)', data: tempData,
      borderColor: '#0ff', borderWidth: 2, pointRadius: 0, tension: 0.25
    }]},
    options: {
      responsive: true, animation: false,
      plugins: { legend: { display:false } },
      scales: commonScales
    }
  });

  chartHum = new Chart(document.getElementById('chartHum'), {
    type: 'line',
    data: { labels, datasets: [{
      label: 'Humidity (%)', data: humData,
      borderColor: '#0ff', borderWidth: 2, pointRadius: 0, tension: 0.25
    }]},
    options: {
      responsive: true, animation: false,
      plugins: { legend: { display:false } },
      scales: commonScales
    }
  });

  // VOC index has no official EPA-style bands like CO2/PM2.5 — the sensor's
  // own baseline is ~100, so it's shown as a plain neutral line rather than
  // threshold-colored.
  chartVOC = new Chart(document.getElementById('chartVOC'), {
    type: 'line',
    data: { labels, datasets: [{
      label: 'VOC Index', data: vocData,
      borderColor: '#0ff', borderWidth: 2, pointRadius: 0, tension: 0.25
    }]},
    options: {
      responsive: true, animation: false,
      plugins: { legend: { display:false } },
      scales: commonScales
    }
  });
}

// Loads (or re-loads) the full history buffer from the device and syncs it
// into the charts. Called once on page open, then again on a timer.
//
// IMPORTANT: this is the ONLY thing that ever writes chart data. There is no
// separate "append a new point every 60s" timer running independently in the
// browser. Two independent clocks — the device's own 60s sample timer, and a
// browser-side timer trying to guess when a new point should appear — will
// always drift apart, since they start counting from different moments (the
// device from boot, the browser from page load) and neither can see the
// other's schedule. That drift is what caused labels/lines to desync the
// longer a page stayed open without a refresh: the browser was inventing its
// own points on its own schedule instead of asking the device what it
// actually recorded. Re-fetching the device's real /history buffer and
// resyncing to it — rather than layering a separate JS-side guess on top —
// removes the second clock entirely, so there's nothing left to drift.
// Currently selected range, in hours. 24 = show everything the device has.
let selectedRangeHours = 24;

// The full set of points last fetched from the device — cached so switching
// ranges just re-slices this in memory, no need to refetch from the device.
let latestPoints = null;

const rangeHeaders = {
  co2: 'CO2', pm: 'Particulate Matter (ug/m3)', voc: 'VOC Index',
  temp: 'Temperature (F)', hum: 'Humidity (%)'
};
function rangeLabel(hours){
  return hours === 24 ? 'Last 24 Hours' : `Last ${hours} Hour${hours === 1 ? '' : 's'}`;
}
function updateHeaders(hours){
  document.getElementById('hdrCO2').textContent  = `${rangeHeaders.co2} · ${rangeLabel(hours)}`;
  document.getElementById('hdrPM').textContent   = `${rangeHeaders.pm} · ${rangeLabel(hours)}`;
  document.getElementById('hdrVOC').textContent  = `${rangeHeaders.voc} · ${rangeLabel(hours)}`;
  document.getElementById('hdrTemp').textContent = `${rangeHeaders.temp} · ${rangeLabel(hours)}`;
  document.getElementById('hdrHum').textContent  = `${rangeHeaders.hum} · ${rangeLabel(hours)}`;
}

// Slices the cached full-history array down to the last N hours. Points are
// sampled once per minute on the device, so N hours = N*60 most recent points.
function sliceToRange(points, hours){
  if (hours >= 24) return points; // 24h = everything the device has
  const count = hours * 60;
  return points.length > count ? points.slice(-count) : points;
}

function renderFromCache(){
  if (!latestPoints) return;
  const points = sliceToRange(latestPoints, selectedRangeHours);

  const labels   = points.map(p => fmtTime(p.t));
  const co2Data  = points.map(p => p.co2);
  const pm1Data  = points.map(p => p.pm1);
  const pm25Data = points.map(p => p.pm25);
  const pm4Data  = points.map(p => p.pm4);
  const pm10Data = points.map(p => p.pm10);
  const tempData = points.map(p => p.temp);
  const humData  = points.map(p => p.hum);
  const vocData  = points.map(p => p.voc);

  updateHeaders(selectedRangeHours);

  if (!chartCO2) {
    buildCharts(labels, co2Data, pm1Data, pm25Data, pm4Data, pm10Data, tempData, humData, vocData);
    return;
  }

  chartCO2.data.labels = labels;
  chartCO2.data.datasets[0].data = co2Data;

  chartPM.data.labels = labels;
  chartPM.data.datasets[0].data = pm1Data;
  chartPM.data.datasets[1].data = pm25Data;
  chartPM.data.datasets[2].data = pm4Data;
  chartPM.data.datasets[3].data = pm10Data;

  chartTemp.data.labels = labels;
  chartTemp.data.datasets[0].data = tempData;

  chartHum.data.labels = labels;
  chartHum.data.datasets[0].data = humData;

  chartVOC.data.labels = labels;
  chartVOC.data.datasets[0].data = vocData;

  [chartCO2, chartPM, chartTemp, chartHum, chartVOC].forEach(ch => ch.update('none'));
}

// Fetches the device's full history buffer and caches it, then renders
// whatever range is currently selected. This is the ONLY thing that ever
// fetches from the device — switching ranges afterward just re-slices the
// cached array, no new fetch needed. See the note above loadHistory's
// previous version: never invent points client-side, only ever display
// exactly what the device's own buffer reports.
function loadHistory(isFirstLoad){
  fetch('/history').then(r=>r.json()).then(points=>{
    latestPoints = points;
    renderFromCache();
  }).catch(e=>{
    if (isFirstLoad) {
      document.querySelectorAll('.chartwrap').forEach(el=>{
        el.innerHTML = "<div class='loading'>History unavailable</div>";
      });
    }
    // On a resync failure (not first load), just skip this cycle and leave
    // the charts showing the last-known-good data — don't blank them out
    // over one missed fetch.
  });
}

document.querySelectorAll('.rangebtn').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('.rangebtn').forEach(b => b.classList.remove('active'));
    btn.classList.add('active');
    selectedRangeHours = parseInt(btn.dataset.hours, 10);
    renderFromCache(); // instant — no fetch needed, just re-slice cached data
  });
});

loadHistory(true);
setInterval(() => loadHistory(false), 60000); // resync from the device's real buffer every 60s

} // end Chart-availability guard
</script></body></html>
)HTML";
