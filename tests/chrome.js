// The Chrome or Chromium that puppeteer-core drives. Set CHROME to choose one.
const fs = require('fs');
const CANDIDATES = [
  process.env.CHROME,
  'C:/Program Files/Google/Chrome/Application/chrome.exe',
  'C:/Program Files (x86)/Google/Chrome/Application/chrome.exe',
  'C:/Program Files/Microsoft/Edge/Application/msedge.exe',
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
  '/opt/pw-browsers/chromium-1194/chrome-linux/chrome',
  '/usr/bin/chromium', '/usr/bin/chromium-browser', '/usr/bin/google-chrome',
];
const found = CANDIDATES.find(p => p && fs.existsSync(p));
if (!found) { console.error('No Chrome found. Set CHROME to its path.'); process.exit(2); }
module.exports = found;
