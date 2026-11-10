# DirectPDF User Manual

A lightweight, open-source PDF reader and markup tool.

---

## Opening a PDF

- **File > Open...** (`Ctrl+O`)
- **Drag and drop** a `.pdf` file onto the window
- **File > Open Recent** — your last several opened files, remembered across restarts

Each opened file gets its own **tab** — Open/Open Recent/drag-and-drop always add a new tab rather than replacing what you already have open, so you can work on several documents at once in one window. Click a tab to switch to it; the Pages/Bookmarks/Markups/Format panels and the toolbar always reflect whichever tab is currently active. Each tab has its own independent undo history, page arrangement, pending bookmarks, and Copy Page clipboard — switching tabs never mixes them up.

- **Close Tab** (`Ctrl+W`, or the tab's close button): closes the current tab, prompting to save first if it has unsaved changes. Closing the last tab leaves the window open and empty (File > Open to start again) rather than quitting the app.

## Reading & Navigation

- **Continuous scroll** through all pages; scroll or use `Page Up` / `Page Down`
- **Zoom**: toolbar buttons, View menu, `Ctrl` + scroll wheel, or shortcuts:
  - `Ctrl++` / `Ctrl+-` — Zoom In / Out
  - `Ctrl+0` — Actual Size
  - `Ctrl+9` — Fit Width
- **Go to page**: type a page number into the box in the status bar (bottom, centered next to the page indicator), or use the ◀ / ▶ buttons on either side to step to the previous/next page. **"Page N" here always means the same N shown under the thumbnail in the Pages panel** — if you've reordered pages by dragging or typing a position number, the counter, the Go to box, and the arrows all follow that new order (counting up 1, 2, 3... in the new order) rather than the page's original number.
- **Rotate view**: `Ctrl+]` (clockwise) / `Ctrl+[` (counterclockwise), or View menu / right-click menu. Written into the file on the next Save (applied to every page); while a rotation is active in the meantime, it's display-only and clicking to select text or images is paused (reset rotation to re-enable).
- **Full Screen**: `Ctrl+Shift+F`, or View menu — toggles native full screen; stays in sync even if you exit via Esc or the window's own full-screen control.
- **Find in document**: `Ctrl+F` opens a find bar at the top of the page. Searches the whole document, including phrases that span multiple words. `Enter` / the down arrow jumps to the next match, `Shift+Enter` / up arrow to the previous. The current match is highlighted in orange. `Esc` or the close button dismisses the bar.

## Format Toolbar

A second toolbar row directly under the main one, always visible, spreading every formatting control for text boxes and shapes out horizontally (font, color, alignment, box styling — see Formatting below) so you can see and adjust everything for whatever's currently selected at a glance, rather than digging into a narrow side panel. Controls enable/disable automatically based on what's selected (e.g. Fill greys out for a line/arrow, which have no interior).

- **View > Format**: hides or shows this toolbar if you want it out of the way.

## Side Panels

Four dockable panels, tabbed together along the **bottom** of the window by default (drag the dock's edge to resize; click the tabs to switch between them). This is just where they start — like any Qt dock widget, you can drag a panel's title bar to any edge of the window (left, right, top, back to bottom) if you prefer a different layout; nothing about them is pinned in place.

- **View > Bottom Panel** (`Ctrl+Shift+B`), or the **▾ Panel / ▴ Panel** button in the status bar: hides or shows the whole panel group in one shot. The status bar button stays visible even while the panel itself is hidden — a permanent reminder that it's just tucked away, and a one-click way to bring it back. Its arrow flips (▾ when shown, ▴ when hidden) to hint which way clicking it will go.
- **View > Pages / Bookmarks / Markups / Tools**: toggle each one individually, if you just want to hide one tab rather than the whole group.

- **Pages** — a thumbnail of every page, bordered, with the current page highlighted in blue. Thumbnails rescale automatically as you resize the panel (drag the panel's edge to make them bigger or smaller). Click a thumbnail to jump to that page — then the **arrow keys** (any of the four) step to the previous/next page in display order, same as the ◀/▶ buttons in the status bar. The thumbnails lay out to match whichever edge the panel is docked to — side-by-side, scrolling left-right, when docked top or bottom (the default); stacked, scrolling up-down, when docked left or right.
- **Bookmarks** — the PDF's own table of contents (if it has one) plus any bookmarks you add. Click an entry to jump to it.
- **Markups** — see Markups below.
- **Tools** — see Toolbox below.

## Toolbox

The **Tools** tab (in the same tabbed panel as Pages/Bookmarks/Markups — see Side Panels above) holds every "place something on the page" tool: **Select Image/Region**, **Add Text**, and the shape tools (**Rectangle**, **Rounded Rectangle**, **Ellipse**, **Diamond**, **Line**, **Arrow**, **Polygon**). All of these are also available from the **Edit** menu. Only one tool is active at a time — turning one on turns off whichever was active before, and clicking an active tool's own button again turns it off and returns to normal selection.

## Selecting & Copying

- **Select text**: click and drag, just like any other app — selection works at the character level (you can start or end mid-word), not just whole words.
- **Select All** (`Ctrl+A`): selects all text on the current page.
- **Copy** (`Ctrl+C`): copies the current selection — text, or an image if one is selected (see below).
- **Select an embedded image**: in normal mode, click directly on a picture/diagram in the page to select just that image, then `Ctrl+C` to copy it (rendered crisp at 300 DPI, not a low-res screen grab).
- **Select Image/Region tool**: toggle it on (toolbar button, or Edit menu) to drag a free rectangle anywhere on the page — like Bluebeam's Snapshot tool — and copy that region as an image. Toggle off to return to normal text selection.
- **Right-click menu**: context-aware — shows Copy or Copy Image depending on what's selected, plus Select All, Add Bookmark, Highlight, Zoom controls, and Rotate.

## Undo & Redo

- **Undo** (`Ctrl+Z`) / **Redo** (`Ctrl+Shift+Z`), also in the Edit menu — the menu entries name what they'll undo/redo (e.g. "Undo Delete Page", "Undo Reorder Pages", "Undo Edit Markup") and grey out when there's nothing left in that direction.
- Covers page operations (reorder, delete, copy/paste, duplicate) and markup operations (adding, removing, or editing a highlight/text box/shape/pasted image).
- Does **not** cover Title/Author edits, bookmarks, outline renames, or rotation — those are infrequent, dialog-driven changes where you'd just change the value back rather than reach for Undo, so they're out of scope for now.
- Undo history is per-tab and is cleared whenever that tab's document is replaced (Save, which reloads the file, or Revert) — it doesn't carry across documents, and switching tabs switches which document's history Undo/Redo act on.

## Bookmarks

- **Add Bookmark** (`Ctrl+D`, toolbar button, or Edit menu): bookmarks the current page. If you have text selected when you do this, the bookmark instead targets that exact spot, defaults its title to a snippet of the selected text, and — when you click it later — scrolls to and **highlights** that exact text in orange.
  - The highlight clears automatically the moment you click elsewhere on the page, or by pressing `Esc`.
  - The highlight itself is a within-this-app convenience — it is not written into the saved PDF (only the page-level jump target is). Reopening the file in a future session will jump to the right page but won't restore the highlight.
- **Rename a bookmark**: right-click it in the Bookmarks panel and choose Rename, or select it and press `F2`. Works both for bookmarks you've added and for the PDF's own original table-of-contents entries. (Double-click is reserved for jumping to the bookmark, so renaming is deliberately not a double-click action.)
- Newly-added, not-yet-saved bookmarks show in *italic* with a "Not yet saved" tooltip until you Save.
- Any page with a bookmark pointing to it (yours, or one from the PDF's own table of contents) gets a small gold ribbon marker in its corner — both on the page itself in the main view, and on its thumbnail in the Pages panel.

## Markups

Every highlight, text box, and shape you place ends up as a row in the **Markups** panel — one place to see everything you've put on the document, jump straight to any of it, and leave yourself a note about it (similar to Bluebeam Revu's markups list).

- The **Markups panel** lists every placed item with four columns: **Type** (Highlight / Text / Shape), **Page**, a **Description** (a text snippet for highlights and text boxes, or the shape's name — "Rectangle", "Arrow", etc. — for shapes), and a **Note** column of your own.
- **Click any row** to jump straight to that item — scrolled into view and selected, not just "somewhere on the right page."
- **Double-click the Note cell** (or press the edit key) to type a comment, then click away or press Enter to save it.
- **Right-click a row** for **Remove**, which deletes both the row and the item itself from the page; a highlight row also offers **Change Highlight Color...**.
- A row appears the moment the item is actually placed — for a text box, that means once you click away or otherwise finish typing (an empty box you abandon without typing anything never appears, same as it's discarded entirely).

Highlights specifically also have a dedicated way to create them:
- **Highlight** (`Ctrl+H`, toolbar button, Edit menu, or right-click menu): select some text first, then trigger this — the selection turns into a highlight on the page and a new row appears in the Markups panel. If nothing is selected, it does nothing (a status bar message reminds you to select text first).
- New highlights default to yellow, but **right-click a highlight's row in the Markups panel → Change Highlight Color...** to recolor it — the color you pick also becomes the default for the next highlight you create, like the border/fill defaults for text boxes and shapes.
- Unlike the brief orange flash a bookmark jump gives you (above), a highlight stays on the page permanently.

### Markups from other PDF tools

Opening a file that already has markups from another program — a highlight made in Preview, a comment added in Acrobat, etc. — lists those too, in the same Markups panel, below your own:

- Shown with their real type (Highlight, Underline, Note, Shape, Freehand, ...) and, as the Description, that annotation's own comment text if it has one, else a snippet of whatever page text it covers.
- **Click a row** to jump to its page, same as any other markup. **Click the annotation directly on the page** (same as clicking an embedded image or one of your own shapes) to select it there instead, then **Delete/Backspace** to remove it — same gesture as every other removable object.
- **Right-click a row → Remove** (or Delete/Backspace after clicking it on the page) deletes it — gone from the page immediately, and from the file's own annotations on your next Save.
- These are read-only otherwise: no Note column (there's no DirectPDF-authored comment to attach to someone else's annotation), and removing one is an immediate, direct edit rather than something **Undo** covers — reconstructing whatever another program's annotation originally was isn't something Undo can faithfully reverse, so treat Remove here as final.
- Just having one of these present doesn't by itself count as an unsaved change — only actually removing one does.

## Placing Objects

- **Paste image** (`Ctrl+V`, or Edit menu): pastes an image from your clipboard onto the current page as a draggable object. Click-drag to move it; select it and press `Delete`/`Backspace` to remove it.

- **Add Text** (`Ctrl+T`, Toolbox, or Edit menu): toggle it on, then click anywhere on the page to place an editable text box — a real text cursor appears immediately, and you type into it exactly like a word processor: normal text selection, backspace, arrow-key navigation, multi-line text with word wrap. Click anywhere else on the page (or press `Esc`) to finish editing — an empty box you click away from without typing anything is discarded automatically. The tool automatically turns itself back off after placing one box, so you're back to normal selection right away instead of stamping down boxes on every click.
  - **The box stays exactly the size you placed or resized it at — it does not grow or shrink while you type.** If your text overflows the box, it's simply clipped (cut off) rather than pushing the box around. Use **Fit Box to Text** or **Size Text to Box** (see Formatting below) to resolve overflow explicitly, on your terms.
  - To edit an existing text box again later, double-click it (in normal mode, tool toggled off).
  - To move one, click-drag it from the middle (single click selects without entering edit mode).
  - **To resize one**: click it to select it, then drag any of the 8 square handles that appear around its edges/corners — side handles resize one dimension (width or height), corner handles resize both at once. Works whether or not you're actively editing the text inside.
  - Select a text box and press `Delete`/`Backspace` to remove it.
  - Rotation pauses editing/placing/resizing text boxes, same as it pauses other click-to-select interactions.
  - **New text boxes start out using whatever font, size, style, and color you last used**, and whatever border/fill you last gave a text box — like a word processor, your formatting carries forward instead of resetting every time. Changing it again (on a box, or while typing) updates that carried-forward default for the *next* new box too.

- **Shapes** (Toolbox, or Edit menu): toggle on **Rectangle**, **Rounded Rectangle**, **Ellipse**, or **Diamond**, then click-drag on the page to place one — press to anchor a corner, drag to size it, release to drop it. (A plain click with no drag places a sensible default-sized shape instead of a sliver.) Like Add Text, the tool automatically turns itself back off after placing one shape.
  - Select, move (click-drag from the middle), resize (8 handles, same as text boxes), and delete (`Delete`/`Backspace`) all work exactly like text boxes.
  - Style a shape's border and fill from the **Format toolbar**'s Box group — same controls as text boxes (see Formatting below).
  - **New shapes start out with whatever border and fill you last gave a shape** — the same color/weight/opacity carries forward to the next one you place (shared across all shape tools — switching shape type doesn't reset it), until you change it again.

- **Line / Arrow** (Toolbox, or Edit menu): same click-drag placement as the other shapes — press where you want it to start, drag, release where you want it to end. For **Arrow**, the arrowhead always points toward wherever you released the drag, so drag in the direction you want it pointing.
  - Select by clicking anywhere along the line (not just its endpoints); move by dragging from the middle, same as other shapes.
  - **Resize by dragging either endpoint** — there are just the two endpoint handles, not the 8-handle box used by other shapes, since a line only has two ends to adjust.
  - Only has a **Border** (color/weight) in the Format toolbar — no Fill, since there's no interior to fill; the Fill controls grey out while a line or arrow is selected.
  - Delete (`Delete`/`Backspace`) and sticky last-used style both work the same as the other shapes.

- **Polygon** (Toolbox, or Edit menu): a different placement gesture from the other shapes — **click each corner in turn**, then **double-click, or press Enter, to close the shape**; press **Esc** to cancel and discard it mid-placement. Needs at least 3 points to form a shape.
  - Select by clicking anywhere inside it; move by dragging from the middle, same as other shapes.
  - **Resize by dragging any individual corner** — each vertex has its own handle, so you can reshape the polygon freely rather than just scaling a bounding box.
  - Has both **Border** and **Fill** in the Format toolbar, same as Rectangle/Ellipse/Diamond.
  - Delete (`Delete`/`Backspace`) and sticky last-used style both work the same as the other shapes.
  - This is a straight-edged polygon, not Acrobat/Revu's bumpy-arc "Cloud" style — the arc rendering is a lot of added geometry for a style choice, so it's out of scope for now; let me know if you want it.

### Formatting text boxes and shapes

The **Format toolbar** (see above) controls both the text itself and the box/shape it sits in. It's live: select any range of text inside a box you're editing and the controls act on just that selection, like Word.

**You don't need to double-click into a text box first** — just select it (single click) and use the Text controls; they apply to the *entire* block of text. (If you want to format only part of the text, double-click in, select that range, then use the same controls — same as editing a selection in Word.)

**Text** (active whenever a text box is selected, open for editing or not):
- Font family and size
- **Bold**, *Italic*, <u>Underline</u>
- Text color and highlight color (click the swatch to pick a color)
- Paragraph alignment — left, center, right, justify
- Bullet lists and numbered lists
- **Fit Box to Text**: resizes the box's height to exactly match its current content (one-shot — it doesn't keep readjusting as you keep typing).
- **Size Text to Box**: the reverse — shrinks the text's font size so the existing content fits within the box's current size, instead of resizing the box. Also one-shot.
- These two sizing buttons only apply to text boxes (not shapes) and are enabled whenever a text box is selected, even if you're not actively editing it — everything else in the Text group needs an open editor.

**Box** (active whenever a text box *or* a shape is selected — the group's title switches between "Text Box" and "Shape" to match):
- Border color and weight (line thickness, in points; `0` = no visible border)
- Fill color and opacity/transparency (`0%` = fully transparent — for text boxes, the default, so it's just floating text with no background; shapes default to a thin visible border and no fill)

New text boxes start with no border and no fill (plain floating text, the most common case) — turn these on from the panel if you want a bordered/filled box like a classic Word text box or a Revu callout.

## Saving

DirectPDF can write real changes back into a PDF file (via the qpdf library — a separate, mature PDF-writing engine from the Poppler library used for reading/rendering).

- **Save** (`Ctrl+S`): writes any pending changes to the current file. Safe to use even when saving over the file you opened — DirectPDF writes to a temporary file first and swaps it in, so a crash or error mid-write can't corrupt your original.
- **Save As...** (`Ctrl+Shift+S`): writes to a new file path and switches to editing that copy.
- If you try to open a different file, or close the app, with unsaved changes pending, you're asked whether to Save, Discard, or Cancel first — nothing is silently thrown away.

**Autosave / crash recovery**: every couple of minutes, DirectPDF quietly writes a full snapshot (same flattening as a real Save) of every open tab that has something unsaved, each to its own recovery file outside your documents — not a substitute for Save, just a safety net. If the app doesn't get to close normally (crash, force quit, power loss), it offers to recover each one (into its own new tab) the next time you launch it. A recovered copy opens as its own file with everything restored; use **Save As** to write it back over your original. Saving or explicitly discarding a tab's changes normally clears its recovery file — it's only ever used after an unclean exit. One caveat: page **labels** specifically are only guaranteed to be protected for whichever tab was active when autosave last ran — labels typed into a background tab since you last switched to it may not make it into that tab's recovery snapshot.

What gets saved:
- Edited **Title** / **Author** (via Document Properties)
- **Page order**, and any page **deletions**/**copies**, if you've changed the page arrangement (see below)
- **New bookmarks** you've added, and any bookmark **renames** (both new and original PDF ones)
- **Placed objects** — pasted images, text boxes, and shapes — and **highlights**, flattened directly into each page's content. Text boxes are saved as real, selectable PDF text (not an image of the text), and shapes/highlights as real vector/overlay content — not just a screenshot of what you placed.
- **Page rotation** (Rotate Clockwise/Counterclockwise) — applied to every page, stacked on top of whatever rotation the page already had.
- **Notes** on highlights/text boxes/shapes — written as real PDF comment (sticky-note) annotations, positioned at the item they belong to. Other PDF viewers can see and read these too, not just DirectPDF.
- **Page labels** (the Pages panel's free-text name field) — written using the PDF format's own page-label mechanism, so other viewers' page navigators show them too. Reopening a file saved by DirectPDF restores them into the Label fields correctly; a label written by some *other* tool is only picked up if it's a plain text label with no page-numbering style attached (DirectPDF won't misread an ordinary roman-numeral/decimal numbering scheme as a label).
- **Removal of a markup from another PDF tool** (see Markups above) — if you removed one, it's gone from the file's own annotations list, not just hidden on screen.

What does *not* currently get saved (planned for later):
- Bookmark text highlights (only the page jump target is saved)

## Document Properties

**File > Properties...** shows Title, Author, Subject, Creator, Producer, page count, PDF version, and file size. Title and Author are editable — your changes are staged and written on the next Save.

## Reordering & Naming Pages

Each thumbnail in the **Pages** panel has two things below/over it — don't mix them up, they do different things:

- **Position number** (small badge over the thumbnail's top-left corner): a plain, read-only indicator of the page's current position — not editable. **Drag a thumbnail onto another one** to reorder — drop on the left/top half of the target to land before it, the right/bottom half to land after it.
- **Label** (directly below the image, placeholder text "Label..."): a free-text name of your own choosing for the page — e.g. "Chapter 1", "Appendix" — completely independent of its position number, and editable by clicking into it. Written into the saved PDF via the format's own page-label mechanism (see Saving) — other PDF viewers' page navigators show it too, not just DirectPDF.

Both the Pages panel and the main reading pane reflect a drag reorder immediately — scroll through the main view and the pages appear in their new order right away, no Save needed to see it. Bookmarks, highlights, and other markups stay attached to the right content since they're tracked by what's actually on the page, not by position. It's still just an in-session arrangement, though: Save is what actually writes the new order into the file.

## Deleting, Copying & Pasting Pages

All from the **Pages** panel — click a thumbnail to select it first:

- **Delete**: `Delete`/`Backspace`, or right-click → **Delete Page**. Refuses (with a status bar message) if it's the only page left — a document can't have zero pages.
- **Copy**: `Ctrl+C`, or right-click → **Copy Page**. Remembers that page as a one-page internal clipboard (copying a new page replaces whatever was copied before). This clipboard is per-tab — it doesn't carry over to a different tab/document; to bring pages in from a different file, use **Insert Pages from File** below instead.
- **Paste**: `Ctrl+V`, or right-click → **Paste Page**. Inserts the copied page immediately after the selected thumbnail. Copy page A, then click over to page B and paste — A lands right after B.
- **Duplicate**: right-click → **Duplicate Page** — copy-and-paste-after-itself in one click, the common case of just wanting a second copy of a page right next to the first.
- The same four actions are also in the **Edit** menu (no keyboard shortcuts there, to avoid clashing with `Ctrl+C`/`Ctrl+V`'s normal text/image meaning) — those act on whichever page is currently open in the main view, regardless of what's selected in the Pages panel.
- `Ctrl+C`/`Ctrl+V` are shared with normal copy/paste: they copy/paste a **page** when a thumbnail has keyboard focus (i.e. you just clicked one), and fall back to the normal text/image/clipboard behavior otherwise.
- Pasting or duplicating adds a new thumbnail for the same page content; deleting removes one. Both are reflected immediately in the Pages panel and the main reading view, the same as reordering.
- Session-only like every other page-order change here — Save is what actually writes it into the file.

## Inserting Pages from Another File

- **Insert Pages from File...** (Edit menu, or right-click a thumbnail in the Pages panel): pick a different PDF, then enter which of its pages to insert (e.g. `1-3,5`) — accepts any mix of single pages and ranges, in any order. The inserted pages land immediately after whichever page is current (or wherever you right-clicked, from the Pages panel).
- **Unlike every other page operation above, this is not deferred until Save** — it writes to your file immediately, the same as clicking Save, and folds in any other pending changes (bookmarks, markups, rotation, etc.) at the same time. It's also not covered by Undo.
- The inserted pages come in as plain copies — any highlights/text boxes/shapes on them in the *original* file aren't preserved (only flattened content baked into the page itself carries over, same as any other PDF page).

## Other File Commands

- **Revert to Saved** (File menu): reloads the current file from disk, discarding any in-session changes (pasted images, pending renames, unsaved bookmarks, etc.), after a confirmation prompt.
- **Print...** (`Ctrl+P`): standard OS print dialog.

## Help Menu

- **Keyboard Shortcuts**: a live-generated list of every shortcut currently bound in the app.
- **User Manual**: this document.
- **About DirectPDF**: version info.

---

## Known Limitations (for now)

- If you both reorder/delete/duplicate pages *and* add a brand-new bookmark in that same Save, the new bookmark can end up on the wrong page — adding bookmarks and rearranging pages in the same session isn't fully reconciled yet. Save one change at a time if you hit this.
- Bookmark text highlights aren't written into the saved PDF (only the page jump target is).
- Multi-page text selection (dragging a selection across a page boundary) isn't supported — selection is confined to the page you start dragging on.

---

*This manual is kept up to date alongside the app — every new feature gets added here as it ships.*
