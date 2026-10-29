// Renders the portal page (PAGE[] from src/web.cpp) with anonymized sample
// API data and writes desktop + iPhone screenshots, reduced to a 32-color
// palette to keep the repository small. No device is needed, and no real
// network names, addresses or credentials end up in the images.
//
// Usage (from the repository root, Node 18+, Google Chrome installed):
//   npm i --no-save playwright sharp
//   node scripts/portal_screenshots.mjs src/web.cpp images
import { chromium, devices } from 'playwright';
import sharp from 'sharp';
import fs from 'fs';

const SRC = process.argv[2];      // path to src/web.cpp
const OUT = process.argv[3];      // output directory
const src = fs.readFileSync(SRC, 'utf8');
const PAGE = src.slice(src.indexOf('R"HTML(') + 7, src.indexOf(')HTML"'));

const now = Math.floor(Date.UTC(2026, 9, 17, 12, 0, 0) / 1000);
// Modeled on the ESP32-C5 + W5500 target (dual-band, chip temperature, W5500
// counters); every name, address and MAC is made up.
const status = {
  prov: 0, wifi: 1, eth: 1, rssi: -57, ch: 36, kbps_up: 7840, kbps_down: 164,
  pkt_up: 48213377, pkt_down: 9120455, drop_up: 41, drop_down: 3,
  wifi_disc: 9, wifi_kicks: 2, disc_r: 8, disc_rs: 'ASSOC_LEAVE', disc_src: 0, disc_ago: 5420,
  wd_probe: 1, wd_reconnects: 0, wd_eth_resets: 0, wd_last_reason: 0, roam_count: 0,
  uptime: 266400, net: 1, fo: 0, client_mac: '02:1a:2b:3c:4d:5e', client_ip: '192.168.1.88',
  client_name: 'ipcam-garden', ssid: 'HomeNetwork', bssid: '02:aa:bb:cc:dd:01',
  at: 0, at_s: 0, at_n: 10, at_r: 0, at_k: 2,
  heap: 98304, heap_min: 61440, heap_dma: 90112, heap_blk: 81920,
  txp: 72, bw: 20, air_ok: 48190210, air_fail: 37, cc: '01', cc_max: 20,
  txq: 3, txq_x10: 28, txq_ms: 2, pt: 0, pt_s: 0, pt_k: 52, pt_r: 0, drop_cap: 0, txq_cap: 0,
  qc: 0, qc_s: 0, qc_k: 0, qc_r: -1, all: 0, lr: 0, lr_k: 0, lr_el: 0, lr_tot: 0, lr_r: 0,
  spi: 80, temp: 642, temp_max: 812, thr: 0, drop_thr: 0,
  w5: [0, 0, 0, 0, 0], w5x: [48213377, 10544120, 6120334, 1460215, 1211002, 98114, 0],
  rst: 3, rtc: 1, time: now, build: '72e00e8',
};
const def = { eth_tx_retries: 8, wifi_tx_retries: 1, static_rx_buf: 16, dynamic_rx_buf: 32,
  dynamic_tx_buf: 32, rx_ba_win: 32, wifi_connect_timeout_s: 20, wifi_connect_retries: 3,
  ap_idle_reboot_s: 900, roam_check_s: 300, roam_rssi_threshold: -70, roam_margin_db: 6,
  wd_actions: 7, wd_bad_windows: 3 };
const config = {
  name: 'wifibridge', ssid1: 'HomeNetwork', ssid2: 'Workshop', ssid3: '',
  ip: '192.168.1.250', mask: '255.255.255.0', gw: '192.168.1.1',
  ip2: '10.0.8.250', mask2: '255.255.255.0', gw2: '10.0.8.1', ip3: '', mask3: '', gw3: '',
  mqtt_host: '192.168.1.10', mqtt_port: 1883, mqtt_user: 'bridge', telemetry_s: 10,
  tx_power: 0, eth_tx_retries: 8, wifi_tx_retries: 1, static_rx_buf: 16, dynamic_rx_buf: 32,
  dynamic_tx_buf: 32, rx_ba_win: 32, ht40: 0, no_11b: 1, wd_enable: 1, wd_actions: 7,
  wd_bad_windows: 3, wifi_connect_timeout_s: 20, wifi_connect_retries: 3, ap_idle_reboot_s: 900,
  roam_enable: 0, roam_check_s: 300, roam_rssi_threshold: -70, roam_margin_db: 6,
  txq_cap: 0, band5g: 1, wifi_band: 0, e2w_max_mbit: 0, thr_off: 0, thr_temp_c: 90,
  def, eff: { static_rx_buf: 16, dynamic_rx_buf: 32, dynamic_tx_buf: 32, rx_ba_win: 32 },
};
const ap = '02:aa:bb:cc:dd:01', none = '00:00:00:00:00:00';
const disconnects = {
  kicks: 2, fails: 7, rest: 0,
  by_reason: [{ r: 8, rs: 'ASSOC_LEAVE', n: 1 }, { r: 200, rs: 'BEACON_TIMEOUT', n: 1 }],
  log: [
    { prev: 0, t: 260980, e: now - 5420, ago: 5420, r: 8, rs: 'ASSOC_LEAVE', src: 0, up: 1, rssi: -59, bssid: ap, down: 4 },
    { prev: 0, t: 184200, e: now - 82200, ago: 82200, r: 2, rs: 'AUTH_EXPIRE', src: 2, up: 1, rssi: -61, bssid: ap, down: 6 },
    { prev: 0, t: 120030, e: now - 146370, ago: 146370, r: 201, rs: 'NO_AP_FOUND', src: 0, up: 0, rssi: -128, bssid: none, down: 3 },
    { prev: 0, t: 120025, e: now - 146375, ago: 146375, r: 201, rs: 'NO_AP_FOUND', src: 0, up: 0, rssi: -128, bssid: none, down: 8 },
    { prev: 0, t: 120011, e: now - 146389, ago: 146389, r: 200, rs: 'BEACON_TIMEOUT', src: 0, up: 1, rssi: -74, bssid: ap, down: 22 },
    { prev: 1, t: 3510, e: now - 270000, ago: -1, r: 8, rs: 'ASSOC_LEAVE', src: 3, up: 1, rssi: -77, bssid: '02:aa:bb:cc:dd:02', down: 5 },
  ],
};
const crashes = { now, recs: [
  { e: now - 266500, up: 604800, rst: 4, f: 1, build: '9df4d8d', task: 'w5500_tsk', pc: '42008a3c',
    reason: 'Load access fault (mcause 5, mtval 0x00000000)', bt: ['42008a12'] },
] };

const routes = {
  '/api/status': status, '/api/config': config, '/api/disconnects': disconnects,
  '/api/crashes': crashes, '/api/coredump': { present: false },
  '/api/autotune': { retries: [], txp: [], cap: [], long: [] },
};

async function open(browser, ctxOpts) {
  const ctx = await browser.newContext({ ...ctxOpts, locale: 'en-GB', timezoneId: 'Europe/Berlin',
                                         colorScheme: 'dark' });
  const page = await ctx.newPage();
  await page.route('**/*', r => {
    const p = new URL(r.request().url()).pathname;
    if (p === '/') return r.fulfill({ contentType: 'text/html; charset=utf-8', body: PAGE });
    if (routes[p]) return r.fulfill({ contentType: 'application/json', body: JSON.stringify(routes[p]) });
    return r.fulfill({ status: 404, body: '' });
  });
  await page.goto('http://wifibridge.local/');
  await page.waitForTimeout(800);
  return { ctx, page };
}

const browser = await chromium.launch({ channel: 'chrome' });

// Desktop: status with the disconnect history expanded, and the full page.
{
  const { ctx, page } = await open(browser, { viewport: { width: 1280, height: 1000 }, deviceScaleFactor: 2 });
  await page.click('#dlbox summary');
  await page.waitForTimeout(500);
  await page.screenshot({ path: `${OUT}/desktop-status.png` });
  await ctx.close();
}
{
  const { ctx, page } = await open(browser, { viewport: { width: 1280, height: 1000 }, deviceScaleFactor: 1 });
  await page.screenshot({ path: `${OUT}/desktop-full.png`, fullPage: true });
  await ctx.close();
}

// iPhone: first screen, full page, and the unsaved-changes notice.
{
  const { ctx, page } = await open(browser, { ...devices['iPhone 14'] });
  await page.screenshot({ path: `${OUT}/iphone-status.png` });
  await page.fill('#eth_tx_retries', '12');
  await page.selectOption('#ht40', '1');
  await page.evaluate(() => document.getElementById('eth_tx_retries').scrollIntoView({block: 'center'}));
  await page.waitForTimeout(300);
  await page.screenshot({ path: `${OUT}/iphone-unsaved.png` });
  await ctx.close();
}

{
  const { ctx, page } = await open(browser, { ...devices['iPhone 14'], deviceScaleFactor: 2 });
  await page.screenshot({ path: `${OUT}/iphone-full.png`, fullPage: true });
  await ctx.close();
}
await browser.close();

// 32-color palette: the dark UI has few colors, so text stays sharp while
// the files shrink to about a quarter.
for (const f of fs.readdirSync(OUT).filter(f => f.endsWith('.png'))) {
  const p = `${OUT}/${f}`;
  const buf = await sharp(p).png({ palette: true, colors: 32, effort: 10, compressionLevel: 9, dither: 0.5 }).toBuffer();
  fs.writeFileSync(p, buf);
  console.log(f, buf.length, 'bytes');
}
