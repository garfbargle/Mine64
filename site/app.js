const launchButton = document.querySelector('#launch-game');
const emulatorStatus = document.querySelector('#emulator-status');
const bezel = document.querySelector('#emulator-bezel');
const placeholder = document.querySelector('#emulator-placeholder');

function launchGame() {
  if (window.mine64Launching) return;
  window.mine64Launching = true;
  launchButton.disabled = true;
  launchButton.textContent = 'LOADING ROM…';
  emulatorStatus.textContent = 'LOADING CARTRIDGE';

  window.EJS_player = '#game';
  window.EJS_core = 'n64';
  window.EJS_gameUrl = new URL('rom/mine64.bin', window.location.href).href;
  window.EJS_pathtodata = 'https://cdn.emulatorjs.org/stable/data/';
  window.EJS_startOnLoaded = true;
  window.EJS_language = 'en-US';

  const emulatorScript = document.createElement('script');
  emulatorScript.src = 'https://cdn.emulatorjs.org/stable/data/loader.js';
  emulatorScript.async = true;
  emulatorScript.onload = () => {
    bezel.classList.add('ejs-loaded');
    placeholder.remove();
    emulatorStatus.textContent = 'RUNNING';
  };
  emulatorScript.onerror = () => {
    window.mine64Launching = false;
    launchButton.disabled = false;
    launchButton.innerHTML = 'TRY AGAIN <span>▶</span>';
    emulatorStatus.textContent = 'PLAYER UNAVAILABLE';
  };
  document.body.append(emulatorScript);
}

launchButton.addEventListener('click', launchGame);
