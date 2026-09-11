# Blue Paws Brand Guide

## Brand direction

Blue Paws should feel friendly, dependable and technically capable. The visual balance is:

- **Friendly:** rounded forms, warm paper backgrounds and the Podge and Carrie illustration.
- **Dependable:** deep navy, restrained typography and uncluttered layouts.
- **Technical:** bright tracking blue, clear status colours and precise interface components.

The identity should never feel clinical, childish or like a generic pet shop.

## Core colour palette

| Role | Name | Hex | Use |
| --- | --- | --- | --- |
| Primary | Tracking Blue | `#1D9BF0` | Primary buttons, links, active controls, map routes and brand highlights |
| Primary dark | Deep Tracking Blue | `#147AC1` | Blue text on light backgrounds, pressed states and accessible small text |
| Secondary | Collar Orange | `#F59E0B` | Calls to action, selected trails, highlights and small brand accents |
| Secondary dark | Deep Collar Orange | `#B85C00` | Orange text on light backgrounds and accessible outlined controls |
| Dark anchor | Signal Navy | `#243C50` | Main dark brand field, favicon background and premium packaging |
| Light anchor | Warm Paper | `#EEE9DF` | Main light-mode background and printed brand field |
| Ink | Charcoal Ink | `#273039` | Primary text in light mode |
| Reverse | Soft White | `#F4F7FA` | Primary text in dark mode and reversed marks |

### Supporting tints

| Role | Hex |
| --- | --- |
| Blue tint | `#DCEFFD` |
| Orange tint | `#FDE8BE` |
| Warm card | `#FBF8F1` |
| Warm panel | `#F5F0E7` |
| Warm hover | `#E8E0D4` |
| Warm border | `#D4CABB` |

Tracking Blue and Collar Orange are co-signature colours, but not equal by area. Use roughly **70% blue and 30% orange** whenever both appear. Orange is an accent, not a general background colour.

## Interface themes

### Light mode

| Token | Hex |
| --- | --- |
| Background | `#EEE9DF` |
| Card | `#FBF8F1` |
| Panel | `#F5F0E7` |
| Hover | `#E8E0D4` |
| Primary text | `#273039` |
| Secondary text | `#66706F` |
| Muted text | `#767E7B` |
| Border | `#D4CABB` |

This existing warm, bone-coloured scheme is distinctive and should be retained. It feels closer to quality writing paper than a cold software dashboard.

### Dark mode

| Token | Hex |
| --- | --- |
| Background | `#243C50` |
| Card | `#2C4A61` |
| Panel | `#20364A` |
| Hover | `#315A73` |
| Primary text | `#F4F7FA` |
| Secondary text | `#A7B6C3` |
| Muted text | `#8294A3` |
| Border | `#31485C` |

Use Signal Navy instead of pure black. It is softer and more visibly blue than the former near-black navy while remaining darker than Tracking Blue and Collar Orange.

### Functional colours

These are interface signals, not core brand colours:

| State | Hex |
| --- | --- |
| Success / at home | `#16A34A` |
| Warning / attention | `#F59E0B` |
| Danger / lost / error | `#DC2626` |
| Debug / specialist | `#7C3AED` |

Do not use orange for ordinary warning states where it would confuse branding with system status. Keep sufficient context through labels and icons.

## Typography

### Recommended family: Nunito Sans

Use **Nunito Sans** throughout the website, app, Home Hub GUI, packaging and marketing. It has soft, rounded construction without looking like a novelty font. It is open source and supports a useful range of weights.

- Display headings and wordmark: Nunito Sans ExtraBold, `800`
- Page and card headings: Nunito Sans Bold, `700`
- Buttons and labels: Nunito Sans SemiBold, `600`
- Body copy: Nunito Sans Regular, `400`
- Technical values: Fira Code Medium, `500`, only where monospacing helps

Use sentence case. Avoid excessive all-caps, except for very short labels or eyebrow text with generous letter spacing.

### Acceptable alternative

**Manrope** is a sharper, more technology-led alternative. It is professional but less warm. Do not mix Nunito Sans and Manrope in the same product.

## Logo system

Blue Paws needs a family of related marks rather than one image used everywhere.

1. **Primary corporate lock-up:** simplified two-paw symbol plus the words “Blue Paws”. Use Tracking Blue for the paw/wordmark and a controlled Collar Orange accent. This is the default website header, stationery and packaging logo.
2. **Compact mark:** the two offset paws with their existing energetic angle. Use for app tiles, social profiles and small packaging panels.
3. **Favicon:** retain the simplified paw geometry on Midnight Navy. At 16 to 32 pixels, avoid highlights, gradients and thin outlines.
4. **Character illustration:** Podge and Carrie with the target and satellite remains a campaign/mascot illustration. Use it for onboarding, exhibition graphics, empty states and “how tracking works”, not as the primary corporate logo.

### Paw-mark improvements

- Preserve the offset geometry: upper/left paw slightly left, lower/right paw slightly right.
- Remove the glossy royal-blue gradient from the old large paw artwork.
- Redraw as clean vector shapes using Tracking Blue with a Collar Orange outline or backing shape.
- Use Signal Navy for outlines rather than pure black.
- Create flat one-colour blue, one-colour white and full-colour masters.
- Ensure the inner gaps remain open at 16 px and when embroidered or printed small.

### Podge and Carrie improvements

- Preserve their recognisable faces, target and satellite story.
- Recolour the tracking elements to Tracking Blue and Collar Orange so the illustration belongs to the corporate system.
- Reduce heavy black outlines to Signal Navy.
- Prepare a simplified version for small screens and retain the detailed version for larger marketing artwork.
- Do not add the “Blue Paws” wording inside the illustration. Pair it with the separate wordmark.

## Usage rules

- Warm Paper is the default light canvas. Pure white is reserved for controls that genuinely need it.
- Signal Navy is the default dark canvas. Avoid pure black except for technical necessities.
- Tracking Blue indicates primary action, selection and connectivity.
- Collar Orange draws attention to one important secondary element at a time.
- Never set normal-sized white text on Tracking Blue or Collar Orange without checking contrast. Use Midnight Navy text on orange. Use Deep Tracking Blue for small blue text on Warm Paper.
- Avoid pet-shop clichés such as multiple bright pastels, bubbly novelty type and excessive paw-print patterns.
- Prefer rounded corners and approachable illustrations, but keep spacing, grids and data presentation disciplined.

## Recommended next assets

1. Vector corporate wordmark and horizontal/stacked lock-ups in SVG.
2. Rebuilt flat two-paw symbol in SVG.
3. Favicon/app-icon set at 16, 32, 180, 192 and 512 px.
4. Recoloured Podge and Carrie master illustration.
5. Shared CSS design-token file for the web GUI and Home Hub.
6. One-page logo clear-space, minimum-size and incorrect-use sheet.

## Repository findings

The current web interface already uses `#EEE9DF`, `#FBF8F1`, `#F5F0E7` and `#1D9BF0`. Its current hover colour, `#243C50`, is now the selected Signal Navy and dark-mode base. The favicon currently uses the older `#0D1B2A` background and should be updated to Signal Navy when the logo asset family is rebuilt.

## Reference CSS tokens

```css
:root {
  --bp-tracking-blue: #1d9bf0;
  --bp-tracking-blue-dark: #147ac1;
  --bp-collar-orange: #f59e0b;
  --bp-collar-orange-dark: #b85c00;
  --bp-signal-navy: #243c50;
  --bp-warm-paper: #eee9df;
  --bp-charcoal-ink: #273039;
  --bp-soft-white: #f4f7fa;

  --bp-bg-primary: #243c50;
  --bp-bg-card: #2c4a61;
  --bp-bg-panel: #20364a;
  --bp-bg-hover: #315a73;
  --bp-text-primary: #f4f7fa;
  --bp-text-secondary: #d7e3ec;
  --bp-text-muted: #b8c8d4;
  --bp-border: #496a82;

  --bp-font-sans: "Nunito Sans", "Segoe UI", sans-serif;
  --bp-font-mono: "Fira Code", Consolas, monospace;
}

body.light {
  --bp-bg-primary: #eee9df;
  --bp-bg-card: #fbf8f1;
  --bp-bg-panel: #f5f0e7;
  --bp-bg-hover: #e8e0d4;
  --bp-text-primary: #273039;
  --bp-text-secondary: #66706f;
  --bp-text-muted: #767e7b;
  --bp-border: #d4cabb;
}
```

