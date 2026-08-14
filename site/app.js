const launchButton = document.querySelector('#launch-game');
const overlay = document.querySelector('#player-overlay');
const closeButton = document.querySelector('#close-game');
const emulatorStatus = document.querySelector('#emulator-status');
const backdrop = document.querySelector('.world-backdrop');
const launchShowcaseButton = document.querySelector('#launch-showcase');
const showcaseOverlay = document.querySelector('#showcase-overlay');
const closeShowcaseButton = document.querySelector('#close-showcase');
const showcaseVideo = document.querySelector('#showcase-video');

function launchGame() {
  overlay.classList.add('open');
  overlay.setAttribute('aria-hidden', 'false');
  document.body.style.overflow = 'hidden';
  if (window.mine64Launching) return;

  window.mine64Launching = true;
  window.EJS_player = '#game';
  window.EJS_core = 'n64';
  window.EJS_gameUrl = new URL('rom/mine64.bin', window.location.href).href;
  window.EJS_pathtodata = 'https://cdn.emulatorjs.org/stable/data/';
  window.EJS_startOnLoaded = true;
  window.EJS_language = 'en-US';

  const emulatorScript = document.createElement('script');
  emulatorScript.src = 'https://cdn.emulatorjs.org/stable/data/loader.js';
  emulatorScript.async = true;
  emulatorScript.onload = () => { emulatorStatus.textContent = 'RUNNING'; };
  emulatorScript.onerror = () => { emulatorStatus.textContent = 'PLAYER UNAVAILABLE'; };
  document.body.append(emulatorScript);
}

function closeGame() {
  overlay.classList.remove('open');
  overlay.setAttribute('aria-hidden', 'true');
  document.body.style.overflow = '';
}

function launchShowcase() {
  showcaseOverlay.classList.add('open');
  showcaseOverlay.setAttribute('aria-hidden', 'false');
  document.body.style.overflow = 'hidden';
  showcaseVideo.play().catch(() => {});
}

function closeShowcase() {
  showcaseOverlay.classList.remove('open');
  showcaseOverlay.setAttribute('aria-hidden', 'true');
  showcaseVideo.pause();
  showcaseVideo.currentTime = 0;
  document.body.style.overflow = '';
}

document.querySelectorAll('.shot').forEach((shot) => {
  shot.addEventListener('click', () => {
    document.querySelector('.shot.active').classList.remove('active');
    shot.classList.add('active');
    backdrop.style.backgroundImage = `url("${shot.dataset.image}")`;
  });
});

launchButton.addEventListener('click', launchGame);
closeButton.addEventListener('click', closeGame);
launchShowcaseButton.addEventListener('click', launchShowcase);
closeShowcaseButton.addEventListener('click', closeShowcase);
document.addEventListener('keydown', (event) => { if (event.key === 'Escape') { closeGame(); closeShowcase(); } });
