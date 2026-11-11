/*
 * Shared configuration for the hosted dashboard (docs/index.html).
 *
 * Copy this file to `site.config.js`, paste one Gemini API key, and rebuild:
 *
 *     cp src/web/site.config.example.js src/web/site.config.js
 *     # edit src/web/site.config.js
 *     python3 src/web/build_site.py
 *
 * The key is then baked into docs/index.html and used automatically by every
 * visitor - one client (you, at build time) sets it for everybody, with no
 * per-browser paste and no localStorage.
 *
 * `site.config.js` is gitignored, but the built docs/index.html DOES contain the
 * key in plain text (that is how a static page can share it). Before publishing:
 *   - restrict the key in Google Cloud Console to the Generative Language API,
 *   - add an HTTP referrer restriction for your site, and
 *   - never bake a key you are not willing to expose to anyone who opens the page.
 *
 * Alternatively, pass it at build time and keep no file at all:
 *     HX_GEMINI_API_KEY=AIza... python3 src/web/build_site.py
 *     python3 src/web/build_site.py --api-key AIza... --model gemini-3.6-flash
 */
window.HX_SITE_CONFIG = {
  // Leave empty to let each visitor paste their own key (stored in their browser).
  apiKey: '',
  // Optional default model; empty keeps the built-in default.
  model: ''
};
