// Dynamically load jQuery and Bootstrap from CDN
(function loadDeps(cb){
  const add = (tag, attrs) => { const e = document.createElement(tag); for (const k in attrs) e[k] = attrs[k]; document.head.appendChild(e); return e; };
  const jq = add('script', { src: 'https://cdn.jsdelivr.net/npm/jquery@3.7.1/dist/jquery.min.js' });
  jq.onload = () => {
    const bsCss = add('link', { rel: 'stylesheet', href: 'https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/css/bootstrap.min.css' });
    // Load our dark theme after Bootstrap so it takes precedence
    const themeCss = add('link', { rel: 'stylesheet', href: '/theme.css' });
    const bs = add('script', { src: 'https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/js/bootstrap.bundle.min.js' });
    bs.onload = cb;
  };
})(initApp);

function initApp(){
  const $ = window.jQuery;
  const ui = {
    form: $('#cfgForm'),
    port: $('#server_port'),
    lsleep: $('#light_sleep_enabled'),
    sleepMs: $('#sleep_interval_ms'),
    awakeMs: $('#awake_after_update_ms'),
    stock: $('#stock_symbol'),
    wlat: $('#weather_latitude'),
    wlon: $('#weather_longitude'),
    wtz: $('#weather_tz'),
    stockFetchMs: $('#stock_fetch_interval_ms'),
    stockClosedMs: $('#stock_fetch_interval_closed_ms'),
    stockPreopenMs: $('#stock_fetch_interval_preopen_ms'),
    w24FetchMs: $('#weather24_fetch_interval_ms'),
    wcurFetchMs: $('#weather_current_fetch_interval_ms'),
    marketCheckMs: $('#market_check_interval_ms'),
    lsleepChunkMs: $('#light_sleep_interval_ms'),
    tz: $('#timezone'),
    soundEnabled: $('#sound_enabled'),
    stockMaxSound: $('#stock_max_sound'),
    stockMinSound: $('#stock_min_sound'),
    audioVolume: $('#audio_volume'),
    volumeValue: $('#volumeValue'),
    logCaptureEnabled: $('#log_capture_enabled'),
    status: {
      rtc: $('#rtc'),
      uptime: $('#uptime'),
      battV: $('#batteryV'),
      battPct: $('#batteryPct'),
      wifiSsid: $('#wifiSsid'),
      wifiIp: $('#wifiIp'),
      wifiRssi: $('#wifiRssi'),
      wifiInfo: $('#wifiInfo'),
      marketStatus: $('#marketStatus'),
      tsStock: $('#tsStock'),
      tsWeather24: $('#tsWeather24'),
      tsWeatherCur: $('#tsWeatherCur'),
      tsMarketCheck: $('#tsMarketCheck')
    },
    btns: {
      refresh: $('#btnRefresh'),
      wipe: $('#btnWipeWifi'),
      restart: $('#btnRestart'),
      wifiInfo: $('#btnWifiInfo'),
      fetchLogs: $('#btnFetchLogs'),
      forceFlush: $('#btnForceFlush'),
      clearLog: $('#btnClearLog')
    },
    logContent: $('#logContent')
  };

  function fetchJSON(url, opts){
    return fetch(url, Object.assign({ headers: { 'Content-Type': 'application/json' } }, opts))
      .then(async r => {
        if (!r.ok) {
          const text = await r.text().catch(()=>"<no body>");
          throw new Error(`${r.status} ${text}`);
        }
        return r.json();
      });
  }

  function loadConfig(){
    fetchJSON('/api/config').then(d => {
      ui.port.val(d.server_port);
      ui.lsleep.val(String(!!d.light_sleep_enabled));
      ui.sleepMs.val(d.sleep_interval_ms);
      ui.awakeMs.val(d.awake_after_update_ms);
      ui.stock.val(d.stock_symbol);
      ui.wlat.val(d.weather_latitude || '');
      ui.wlon.val(d.weather_longitude || '');
      ui.wtz.val(d.weather_tz || '');
      ui.stockFetchMs.val(d.stock_fetch_interval_ms || 60000);
      ui.stockClosedMs.val(d.stock_fetch_interval_closed_ms || 0);
      ui.stockPreopenMs.val(d.stock_fetch_interval_preopen_ms || 60000);
      ui.w24FetchMs.val(d.weather24_fetch_interval_ms || (12*60*60*1000));
      ui.wcurFetchMs.val(d.weather_current_fetch_interval_ms || (15*60*1000));
      ui.marketCheckMs.val(d.market_check_interval_ms || (60*60*1000));
      ui.lsleepChunkMs.val(d.light_sleep_interval_ms || 500);
      ui.tz.val(d.timezone);
      ui.soundEnabled.val(String(!!d.sound_enabled));
      // Format to 2 decimals to avoid binary float tails
      ui.stockMaxSound.val((d.stock_max_sound != null ? Number(d.stock_max_sound).toFixed(2) : '0'));
      ui.stockMinSound.val((d.stock_min_sound != null ? Number(d.stock_min_sound).toFixed(2) : '0'));
      const vol = d.audio_volume || 70;
      ui.audioVolume.val(vol);
      ui.volumeValue.text(vol + '%');
      ui.logCaptureEnabled.prop('checked', !!d.log_capture_enabled);
    }).catch(console.error);
  }

  function loadStatus(){
    fetchJSON('/api/status').then(s => {
      ui.status.rtc.text(s.rtc || '-');
      ui.status.uptime.text(s.uptime || '-');
      if (typeof s.battery_voltage_v === 'number') {
        ui.status.battV.text(s.battery_voltage_v.toFixed(2) + 'V');
      } else {
        ui.status.battV.text('-');
      }
      if (typeof s.battery === 'number') {
        ui.status.battPct.text(String(s.battery));
      } else {
        ui.status.battPct.text('-');
      }
      if (s.wifi) {
        ui.status.wifiSsid.text(s.wifi.ssid || '-');
        ui.status.wifiIp.text(s.wifi.ip || '-');
        ui.status.wifiRssi.text(typeof s.wifi.rssi === 'number' ? s.wifi.rssi : '-');
      }
      // Market status
      ui.status.marketStatus.text(s.market_open ? 'OPEN' : 'CLOSED');
      ui.status.marketStatus.css('color', s.market_open ? '#0f0' : '#f00');
      // Timestamps
      const formatTs = (ts) => ts > 0 ? new Date(ts * 1000).toLocaleString() : 'Never';
      if (s.timestamps) {
        ui.status.tsStock.text(formatTs(s.timestamps.stock_last_fetch));
        ui.status.tsWeather24.text(formatTs(s.timestamps.weather24_last_fetch));
        ui.status.tsWeatherCur.text(formatTs(s.timestamps.weather_current_last_fetch));
        ui.status.tsMarketCheck.text(formatTs(s.timestamps.market_last_check));
      }
    }).catch(console.error);
  }

  ui.form.on('submit', function(ev){
    ev.preventDefault();
    const payload = {
      server_port: Number(ui.port.val()),
      light_sleep_enabled: (ui.lsleep.val() === 'true'),
      sleep_interval_ms: Number(ui.sleepMs.val()),
      awake_after_update_ms: Number(ui.awakeMs.val()),
      stock_symbol: String(ui.stock.val() || '').trim(),
      weather_latitude: String(ui.wlat.val() || '').trim(),
      weather_longitude: String(ui.wlon.val() || '').trim(),
      weather_tz: String(ui.wtz.val() || '').trim(),
      stock_fetch_interval_ms: Number(ui.stockFetchMs.val()) || 60000,
      stock_fetch_interval_closed_ms: Number(ui.stockClosedMs.val()) || 0,
      stock_fetch_interval_preopen_ms: Number(ui.stockPreopenMs.val()) || 60000,
      weather24_fetch_interval_ms: Number(ui.w24FetchMs.val()) || (12*60*60*1000),
      weather_current_fetch_interval_ms: Number(ui.wcurFetchMs.val()) || (15*60*1000),
      market_check_interval_ms: Number(ui.marketCheckMs.val()) || (60*60*1000),
      light_sleep_interval_ms: Number(ui.lsleepChunkMs.val()) || 500,
      timezone: String(ui.tz.val() || '').trim(),
      sound_enabled: (ui.soundEnabled.val() === 'true'),
      stock_max_sound: Number(ui.stockMaxSound.val()) || 0,
      stock_min_sound: Number(ui.stockMinSound.val()) || 0,
      audio_volume: Number(ui.audioVolume.val()) || 70,
      log_capture_enabled: ui.logCaptureEnabled.is(':checked'),
    };
    fetchJSON('/api/config', { method: 'POST', body: JSON.stringify(payload) })
      .then(_ => {
        // If port changed, user will need to browse new port
        loadConfig();
      })
      .catch(console.error);
  });

  ui.btns.refresh.on('click', function(){ fetchJSON('/api/refresh', { method:'POST', body:'{}' }); });
  ui.btns.wipe.on('click', function(){ if (confirm('Wipe stored WiFi and reboot later?')) fetchJSON('/api/wifi/wipe', { method:'POST', body:'{}' }); });
  ui.btns.restart.on('click', function(){ if (confirm('Restart device now?')) fetchJSON('/api/restart', { method:'POST', body:'{}' }); });
  ui.btns.wifiInfo.on('click', function(){
    fetchJSON('/api/wifi/info').then(d => {
      ui.status.wifiInfo.text(JSON.stringify(d, null, 2)).show();
    });
  });
  // Test sound button
  $('#btnTestSound').on('click', function(){
    console.log('Test sound button clicked');
    fetchJSON('/api/test_sound', { method: 'POST', body: '{}' })
      .then(result => {
        console.log('Test sound response:', result);
        if (result.status === 'ok') {
          alert('Test sound playing!');
        } else if (result.status === 'sound_disabled') {
          alert('Sounds are disabled. Enable them in Audio Settings first.');
        } else {
          alert('Test sound failed: ' + (result.message || result.status));
        }
      })
      .catch(err => {
        console.error('Test sound failed:', err);
        alert('Test sound request failed: ' + err.message);
      });
  });
  ui.btns.fetchLogs.on('click', function(){
    fetch('/api/log')
      .then(r => r.ok ? r.text() : Promise.reject(`${r.status} ${r.statusText}`))
      .then(text => {
        ui.logContent.val(text);
      })
      .catch(err => {
        console.error('Failed to fetch logs:', err);
        ui.logContent.val('Error fetching logs: ' + err);
      });
  });
  ui.btns.forceFlush.on('click', function(){
    fetchJSON('/api/force_flush', { method:'POST', body:'{}' })
      .then(() => console.log('Log buffer flushed to file'))
      .catch(err => console.error('Flush failed:', err));
  });
  ui.btns.clearLog.on('click', function(){
    fetchJSON('/api/clear_log', { method:'POST', body:'{}' })
      .then(() => console.log('Log file cleared'))
      .catch(err => console.error('Clear failed:', err));
  });

  // Update volume display as slider moves
  ui.audioVolume.on('input', function(){
    const vol = $(this).val();
    ui.volumeValue.text(vol + '%');
  });

  loadConfig();
  loadStatus();
  setInterval(loadStatus, 5000);
}
