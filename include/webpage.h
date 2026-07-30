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
<link rel='icon' type='image/png' href='data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAYAAABzenr0AAAFL0lEQVR4AeyTC0xTVxiAz330tqUtttgWWhCd4EKHOAEnyHDq5jbZfMeC6UDRqCyEaXRmZnMxJGZuuqib8zGQbaIzw4pRa6ISo+imxgcgoqA8LVkrBfq+fd723rvWUKO0ZbKYLFu4Of/9z/nP//jy5z8w+Je/EYCRDvw3O8Cq7cxmXbOWcS/ZaiPP2VR8lWG9oKxu1D95UMPrgFKJIM09B0hx9CmIpizAje+HvO4zAGA5UGzKA8Gx7jeHCzEsADgtYxvNRqdhfS0Tndn8z2w5kuOWeaMPmufz3qNthlKILVEJKu/HDwfixQFu3IgDHFYJo08jt8+aqhtcxLRUWg48jmqEG7958N1Q578FwJpvJcPa9lJknKQSoojf3Zmp7eESIib1jzSDrRh9yn5BWG3ZKT7UnhDON2AfCgBC+1q/8kqjb0IwNI72eE4iuHlTIDCU7l81+S7s0q8AhO0wDYCY4o+9M7pKuyCUb8AWFgDtubuRxuA8hk6XRkoSC6kxiXuJpJSmQKBfF/R9UljQsXaifz8gVH+upNqQG33EsGRUAcB1+RBLVCktqws7FyEBRMp9XJjD/IKpfix3yzLaBpI/p5b1FuWjUVAFHYfWzG9en/jc5cBBXxCvAl7nb4QosWTAFKRCAhDpKRmIl3jomDzjTlCEz6B4XLyY5HO+t+HYFRscwXOOF9akPtwuTdHtX/NaR8XzMKRJCUGMsM8zJACJIXyIJjW+Wk+XvFbOXW0uOpmvK1lH8Vjb8Q7PbDvNdhJ/klP68YitrviYeiKSW+yIiVTGNPwsCgQiDqveNw9I4DxYhwRADZYuANOvBpxn1pai3FTRMQ+FiCkes9ChJWa4LWg33WZWnE34uq1RvP4QbmAsxS3c895eYjk0RrRbWlcW4Y/PoL5Tw6ZOhX8fSkICmCd/0EhDgOJ0XX7XHySUWXbYbWilwwKf9jzAZ6qS9j6uydptPJf5gxXs2sWOUjUm266+0g3s8D6gJSGmRl+CcXhyUFoKp82j1xVP3Orw5YF8ErRCAvi8aNhoLqQ4rIXvN29a4LBjvygroqr7Ltv2HJ9SbvEnRu51fog2mU+yslY/cnMT91KCSUcdrW+fcOmmu9WpK8xqmeLwymXWFBJmEIVwm0JzPflLX96gFRIgfc0aBs9oioo9dmuD/qrpwtmE7fd8Rambslwhqm7eCK/6uBVERG6hXdaqiPqLbzBxfD/DYd6B6rqLAcb/yVcFiu26Kv/DkI4n6bW3TQh2g3Z4byuVIGgWQgLUl5d7aC5X3Z+X+uvDvNWX2NaGK5i5qcsjm9BOY8xpsKG3CGvRLmZQkbMI2SwVjTCEEIVQAEMnAAhtiT1YG+tyCvSZktZFDJfTTkIEkwFovsmUHlQvyOCjf7J4da0G+hG+E1y7v5Dut2+mNcY87+lTQoaudwPgiHOICeMbaMrdaauqmmqaKzygXxJzRl8gqwKki0egwMvq6dE0kVnn9UyBTIp7UtpdzJqionrPk+TP/MICdOSvtZJiroN8XbaNEoz6lIqN3gTnLLpOSOMbKMjLxrT309xZ0d+C8iJP3C5llPhI00ciZfc2hCQq3BIx4bazeiCjU1ssO1Epnd6yZ/rce6Zn6j7dhgXwe9jHv9XkistcyWy4swruNXyDPupQUFu2xlAJY0qc72Rp/T5+0WzINfYVTDranzv281553EXrnGSjfkE23pi6yOy/H0qGBAgE4rOXGzxJ2XXE1DnNwDcfAfvL0C8E8DIKhcsxAjDSgf9/B8JNf8D+FwAAAP//RgyyDwAAAAZJREFUAwDJ4CpfgHqaDwAAAABJRU5ErkJggg=='>
<link rel='apple-touch-icon' href='data:image/png;base64,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'>
<meta name='apple-mobile-web-app-capable' content='yes'>
<meta name='apple-mobile-web-app-title' content='Omni-Core'>
<meta name='apple-mobile-web-app-status-bar-style' content='black-translucent'>
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
