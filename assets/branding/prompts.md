# Production branding prompts

Built-in image generation tool, edit mode, with transparency enabled.
Both edits reference `approved-concept.png`, the preview approved by the user.
Image generation does not run during application builds. ImageMagick and `3it`
export the saved sources through `tools/prepare-branding.py`.

## Wordmark (`../logo-source.png`)

Edit target: the supplied approved OH MY 3DO logo concept sheet. Deliver ONLY the large primary horizontal logo from the top of this sheet as one finished production asset with a genuinely transparent background. Remove the background, divider, bottom repeated logo, and icon tile completely. Keep its exact identity: broad forward-leaning ivory OH MY and lime 3DO letters, tilted optical disc behind the left end, ivory/slate/orange disc wedges, black/navy disc hub, two orange signal rays. Refine minimally for readability at a 148-pixel-wide console size: make the final character an unambiguous uppercase O with a clean closed outline and inner counter, with no Q-like tail, retaining the bold blocky italic custom lettering. Exact text OH MY 3DO, with letter O at the end. Simplify accidental ragged stairs into intentional strong angular edges. Clean flat colors #FFFFEF, #84FF21, #FF7B18, #52526B, #080810. No gradients, texture, glow, shadows outside the silhouette, additional words, mockup, or border. Crop close to this single horizontal mark with small transparent padding; entire disc, rays, letters must fit. Wide landscape asset. Preserve composition, relative sizes, and approved style.

## Disc icon (`../app-icon-source.png`)

Edit target: supplied approved OH MY 3DO branding concept sheet. Isolate and finish ONLY the standalone optical-disc app icon from the bottom right. Deliver one square app icon, large centered tilted optical disc with thick easily readable ivory, muted slate #52526B, and orange #FF7B18 wedge sections, clear navy center hole with ivory hub ring, and the same two orange signal rays above its upper right. Keep the approved emblem's angle and design. Remove all lettering, other logos, divider, and presentation background. The icon sits on a simple near-black navy #101019 rounded-square tile with gently rounded corners, filling the square canvas, transparent only outside the rounded tile corners. Allow consistent generous padding around disc and rays inside the tile (about 10%). Use flat solid colors, clean simplified angular edges readable at 32 pixels, no gradients, no texture, no glow, no metallic chrome, no shadows, no tiny detail, no text. Exact purpose: native Windows and Linux desktop app icon matching the supplied branding.
