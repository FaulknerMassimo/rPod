// Runs before the first paint: puts back the theme rPod had last time, so a
// Classic rPod's page doesn't flash Glass while app.js asks for the real one.
(function () {
  'use strict';
  var t = null;
  try {
    t = localStorage.getItem('rpod-theme');
  } catch (e) {
    // Storage blocked: the page just starts in Glass.
  }
  if (t === 'glass' || t === 'classic' || t === 'classic-dark') {
    document.documentElement.dataset.theme = t;
    document.documentElement.dataset.look = t === 'glass' ? 'glass' : 'classic';
  }
})();
