# BluePaws online-only PWA

The installed app is the existing Vercel/Supabase web GUI in a standalone window.
It requires an internet connection. It does not implement Home Hub offline mode,
register a service worker, cache private telemetry, or queue commands offline.

Open **Account → Install BluePaws**, or visit `/install`. Chromium can offer a
native install button once browser eligibility and engagement checks pass.
Other browsers get installation instructions, including Safari's Add to Home Screen.
The manifest and install page are public; tracker data remains authenticated.

Keep the production origin stable: an installation belongs to its origin.
The manifest ID, launch URL and scope are `/`; preview deployments are separate apps.
Normal website deployments update the app on subsequent loads. An already-open
window may need reloading, just like a browser tab.

## Verification

- Run `npm run test:pwa`, `npm run typecheck`, and the usual web tests.
- Check `/manifest.webmanifest` and all icons return 200 without authentication.
- In Chrome DevTools, check Application → Manifest for installability errors.
- Test account-menu navigation, available/dismissed install prompt, already-installed
  state, and a narrow phone viewport.
- On real Android Chrome and iOS Safari, install, launch, sign in, reopen, resume
  after backgrounding, and check raster/vector maps and tracker actions.
- With connectivity removed, do not expect offline tracking or a cached launch.
  Restore connectivity and verify the existing live reconnection behaviour.

Icons are rasterised from existing brand artwork by
`node scripts/generate-pwa-icons.mjs` (requires `sharp`, or a tools runtime exposing
it through `NODE_PATH`). Outputs are committed, so builds need no generator.
