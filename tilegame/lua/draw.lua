-- DRAW: the sprite and tile editor. It draws the tiles, sprites, palette, tile flags and animation tables of a graphics file (TGF: what loadgfx
-- reads and savegfx writes). RUN it and give the file's name; a file that is not there is made, with tiles of 8 or 16 pixels.
--
-- The mouse: the left button draws with colour A, the right one with colour B (see-through at first, so it rubs out), the middle one picks up the
-- colour under it. Click a tile or sprite in the sheet on the right to draw it (or a tile of the group of four, 4n to 4n + 3, shown two by two
-- above the tile three by three), a colour in the palette to make it A (left) or B (right), a letter along the bottom for what it stands for. The wheel turns the sheet's pages (tiles of 16 pixels), and changes colour A over the palette and an
-- animation frame's time over the frames. Without a mouse the arrow keys move the pointer (a pixel at a time over the drawing), space is the left
-- button and Enter the right one.
--
-- Keys: P pen  F fill  L line  R rectangle  B box (filled)  K pick a colour up  (a line, rectangle or box from the keys: space at each end)
--       X flip across  Y flip up and down  T turn  W A S D shift (round)  C copy  V paste  Delete clear  Z undo  Shift+Z redo  G grid
--       U what the see-through pixels show: grey checks, dark checks, black, white, yellow, red, blue (Shift+U, or the right button, back)
--       , . the tile before, after  [ ] colour A before, after  E edit colour A's red, green and blue (arrows, Page Up and Down; Enter ends)
--       1 to 8 the tile's flags (1 solid, 2 one-way, 3 trigger, 4 to 8 the game's own)
--       F1 tiles  F2 sprites  F3 animation tables: < > the table, M its mode, + add the sprite, - take the frame out, [ ] the frame's time
--       F4 or ? these keys, a page of them (with tiles of 16 pixels, four pages: Page Up and Down, the arrows, the wheel or a click turn them;
--       F4, ? or Escape goes back)
--       Ctrl+S save  Ctrl+O open another file  Escape leave
--
-- The editor's text needs some letters in the file's tiles: if the file has none, it puts them in tiles that are empty (transparent, no flags), and
-- they are saved with the file; if there are not enough empty tiles, it uses working tiles, which are never saved. Its colours are the standard
-- ones, 248 to 255.

local CHARS = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789#*<>+-[]:./?!_,"

-- The keys tab (F4): a line each, "=" before a heading; 19 characters at most, all of them in CHARS. A heading and its lines stay together, in a
-- column of their own if they do not fit in what is left of one (two columns on a page with tiles of 8 pixels, one with tiles of 16).
local KEYS = {
  "=MOUSE",
  "LEFT   DRAW WITH A",
  "RIGHT  DRAW WITH B",
  "MIDDLE PICK COLOUR",
  "WHEEL  PAGE/COLOUR",
  "=NO MOUSE",
  "ARROWS MOVE POINTER",
  "SPACE  LEFT BUTTON",
  "ENTER  RIGHT BUTTON",
  "=TOOLS",
  "P PEN   F FILL",
  "L LINE  R RECTANGLE",
  "B BOX   K PICK",
  "=WHOLE TILE/SPRITE",
  "X FLIP ACROSS",
  "Y FLIP UP/DOWN",
  "T TURN  C COPY",
  "V PASTE DEL CLEAR",
  "WASD SHIFT ROUND",
  "Z UNDO",
  "SHIFT+Z REDO",
  "=VIEW",
  "G GRID",
  "U BACKGROUND",
  "=COLOURS",
  "[ ] COLOUR A -/+",
  "E   EDIT COLOUR A",
  "=EDITING COLOUR A",
  "LEFT RIGHT R/G/B",
  "UP DOWN    BY 1",
  "PGUP PGDN  BY 16",
  "ENTER      DONE",
  "=TILE OR SPRITE",
  ", .  BEFORE/AFTER",
  "1-8 FLAGS: 1 SOLID",
  "2 ONE-WAY 3 TRIGGER",
  "4-8 FOR THE GAME",
  "=TABS",
  "F1 TILES F2 SPRITES",
  "F3 ANIMATION",
  "F4 OR ? THESE KEYS",
  "TAB THE NEXT ONE",
  "=ANIMATION F3",
  "+ ADD   - TAKE OUT",
  "[ ] FRAME TIME",
  "M MODE  < > TABLE",
  "=FILE",
  "CTRL+S SAVE",
  "CTRL+O OPEN ANOTHER",
  "ESC    LEAVE",
}
local BLACK, DARK, GREY, LIGHT, WHITE, YELLOW, RED, BLUE = 248, 249, 250, 251, 252, 253, 254, 255

-- ---- the file ----

-- A TGF file's tile size: nil if there is no such file, false if it is not a TGF file.
local function tgf_size(name)
  local f = io.open(name, "r")
  if not f then return nil end
  local h = f:read(16)
  f:close()
  if h and #h == 16 and h:sub(1, 8) == "TGACTGF1" and (h:byte(13) == 8 or h:byte(13) == 16) then
    return h:byte(13)
  end
  return false
end

-- The file to draw, asked for on the text screen: its name, its tile size and whether it is there; nil if it cannot be.
local function ask_file(default)
  io.write("File to draw (Enter: " .. default .. "): ")
  local name = io.read()
  if not name or name == "" then name = default end
  local size = tgf_size(name)
  if size == false then
    print(name .. " is not a graphics file (TGF).")
    return nil
  end
  if size then return name, size, true end
  io.write("A new file. Tiles of 8 or 16 pixels (Enter: 8): ")
  local s = io.read()
  return name, (s == "16") and 16 or 8, false
end

-- ---- the state ----

local S, Z, CELLS                  -- the tile size; the drawing's zoom (128 pixels across) and its cells a side
local SHEETC                       -- the sheet's columns and rows on the screen (all 256 tiles of 8 pixels, a page of 64 of 16)
local W_CANVAS, W_PAL, W_SHEET, W_FRAMES, W_FONT   -- working tiles: the drawing, the palette, the copies of sprites, of frames, a font
local T_BLANK, T_PANEL, T_SEL, T_POINTER, T_FRAME, T_BOX0, T_BOX1, T_A, T_B, T_CHECK
local file, dirty
local tab = "tile"                 -- "tile", "spr" or "anim" (the sprites, and their animation tables)
local cur = {tile = 1, spr = 0}    -- the tile and the sprite being drawn
local top = {tile = 0, spr = 0}    -- the sheet's first row on the screen
local A, B = 70, 0                 -- the colours of the left and right buttons (70 is "F", white at first)
local tool = "P"
local grid = true
-- What the see-through pixels show (U changes it): the two colours of the checks (one colour twice: no checks), and the grid's colour.
local BACKS = {
  {"GREY CHECKS", GREY, LIGHT, DARK},
  {"DARK CHECKS", BLACK, DARK, GREY},
  {"BLACK", BLACK, BLACK, DARK},
  {"WHITE", WHITE, WHITE, DARK},
  {"YELLOW", YELLOW, YELLOW, DARK},
  {"RED", RED, RED, DARK},
  {"BLUE", BLUE, BLUE, DARK},
}
local back = 1
local pix, over = {}, {}           -- the pixels of what is being drawn (1 = top left), and what a line or box being dragged puts over them
local rowN, rowG = {}, {}          -- the drawing's rows as the screen shows them: a plain row and a grid line, for each pixel row
local undo, redo = {}, {}
local clip                         -- what C copied
local msg, msg_until = "", 0
local mx, my = 160, 120            -- the pointer
local stroke                       -- the drawing going on (a button held on the drawing)
local pending                      -- a line or box started from the keys: its first end
local rgb_edit, chan = false, 1    -- the colour A editor, and which of red, green, blue
local rgb_before                   -- colour A as it was when the editor opened
local ask                          -- a question on the status line: "quit" or "open"
local anim_n, anim_sel = 1, 1      -- the animation table, and its frame
local actor_id
local quit = false
local keys_page                    -- the page of the keys tab being shown (F4), over the tab `tab`; nil when it is not
local key_places, key_pages        -- where each line of KEYS goes, {page, x, y}, and how many pages there are
local shown_status                 -- what the status line shows (nil: it must be drawn again)

-- The working tiles, from 256: where each part begins (with 64 for letters if the file has no room for them). Returns how many there are.
local function plan(size, font_tiles)
  S = size
  Z = 128 // S
  CELLS = 128 // S
  SHEETC = 128 // S
  W_CANVAS = 256
  W_PAL = W_CANVAS + CELLS * CELLS
  W_SHEET = W_PAL + (128 // S) * (64 // S)
  W_FRAMES = W_SHEET + SHEETC * SHEETC
  local u = W_FRAMES + 32
  T_BLANK, T_PANEL, T_SEL, T_POINTER, T_FRAME, T_BOX0, T_BOX1, T_A, T_B, T_CHECK = u, u + 1, u + 2, u + 3, u + 4, u + 5, u + 6, u + 7, u + 8, u + 9
  W_FONT = u + 16
  return W_FONT + font_tiles - 256
end

local function ch(c) return string.char(c) end
local function solid(c) return string.rep(ch(c), S * S) end

local function checker()
  local t, b = {}, BACKS[back]
  for y = 0, S - 1 do
    for x = 0, S - 1 do
      t[#t + 1] = ch(((x // 4) + (y // 4)) % 2 == 0 and b[2] or b[3])
    end
  end
  return table.concat(t)
end

-- A square the size of a tile: an edge a pixel wide, and inside it.
local function box(edge, inside)
  local t = {}
  for y = 0, S - 1 do
    for x = 0, S - 1 do
      local e = x == 0 or y == 0 or x == S - 1 or y == S - 1
      t[#t + 1] = ch(e and edge or inside)
    end
  end
  return table.concat(t)
end

local ARROW = {"X.......", "XX......", "XWX.....", "XWWX....", "XWWWX...", "XWXX....", "XX.XX...", "....X..."}
local function arrow()
  local t, k = {}, S // 8
  for y = 0, S - 1 do
    local r = ARROW[y // k + 1]
    for x = 0, S - 1 do
      local c = r:sub(x // k + 1, x // k + 1)
      t[#t + 1] = ch(c == "X" and BLACK or c == "W" and WHITE or 0)
    end
  end
  return table.concat(t)
end

-- ---- text ----

-- Text on layer 2 at the pixel x, y (in characters of a tile each), after clearing `width` characters there.
local function say(x, y, s, width)
  layer(2)
  local cx, cy = x // S, y // S
  if width and width > 0 then tilefill(cx, cy, width, 1, T_BLANK) end
  text(cx, cy, s:upper())
end

local function status(s, seconds)
  msg, msg_until = s, millis() + (seconds or 2) * 1000
end

-- ---- the drawing ----

local function kind() return tab == "tile" and "tile" or "spr" end
local function item() return cur[kind()] end

local function item_string()
  if tab == "tile" then return tiledef(cur.tile) end
  return sprdef(cur.spr)
end

local function load_pix()
  local s = item_string()
  for i = 1, S * S do pix[i] = s:byte(i) end
  over = {}
end

local function px_at(x, y)
  local i = y * S + x + 1
  return over[i] or pix[i]
end

local function make_row(sy)
  local n, g, b = {}, {}, BACKS[back]
  local gc = ch(b[4])
  for sx = 0, S - 1 do
    local c = px_at(sx, sy)
    if c == 0 then c = (sx + sy) % 2 == 0 and b[2] or b[3] end
    local cc = ch(c)
    n[#n + 1] = grid and gc .. string.rep(cc, Z - 1) or string.rep(cc, Z)
    g[#g + 1] = string.rep(grid and gc or cc, Z)
  end
  rowN[sy], rowG[sy] = table.concat(n), table.concat(g)
end

local function render_cell(cx, cy)
  local t, x0 = {}, cx * S + 1
  for py = 0, S - 1 do
    local Y = cy * S + py
    local r = (grid and Y % Z == 0) and rowG[Y // Z] or rowN[Y // Z]
    t[#t + 1] = r:sub(x0, x0 + S - 1)
  end
  tiledef(W_CANVAS + cy * CELLS + cx, table.concat(t))
end

local function draw_canvas()
  for sy = 0, S - 1 do make_row(sy) end
  for cy = 0, CELLS - 1 do
    for cx = 0, CELLS - 1 do render_cell(cx, cy) end
  end
end

-- One pixel changed: the cells that show it.
local function draw_pixel(x, y)
  make_row(y)
  for cy = (y * Z) // S, ((y + 1) * Z - 1) // S do
    for cx = (x * Z) // S, ((x + 1) * Z - 1) // S do render_cell(cx, cy) end
  end
end

-- ---- the screen ----

local function fill1(x, y, w, h, t)   -- layer 1, in pixels
  layer(1)
  tilefill(x // S, y // S, w // S, h // S, t)
end

local function draw_tabs()
  local names = {"TIL", "SPR", "ANI", "KEY"}
  local on = keys_page and 4 or ({tile = 1, spr = 2, anim = 3})[tab]
  for i = 1, 4 do
    fill1((i - 1) * 4 * S, 0, 3 * S, S, on == i and T_SEL or T_PANEL)
    say((i - 1) * 4 * S, 0, names[i])
  end
  local num = string.format(S == 8 and "#%03d" or "%03d", item()) .. (dirty and "*" or " ")   -- (no # with tiles of 16: a space after KEY)
  say(320 - #num * S, 0, num, #num)
end

local function draw_selection()
  layer(3)
  tilefill(192 // S, 16 // S, SHEETC, SHEETC, T_BLANK)
  local row = item() // SHEETC - top[kind()]
  if row >= 0 and row < SHEETC then
    tile(192 // S + item() % SHEETC, 16 // S + row, T_FRAME)
  end
end

local function sheet_copy(n)          -- a sprite into its place on the sheet, if it is on the screen
  local row = n // SHEETC - top.spr
  if tab ~= "tile" and row >= 0 and row < SHEETC then
    tiledef(W_SHEET + row * SHEETC + n % SHEETC, sprdef(n))
  end
end

-- The sheet and the previews are on layer 2, over a checker on layer 1 that shows what is see-through.
local function draw_sheet()
  layer(1)
  tilefill(192 // S, 16 // S, SHEETC, SHEETC, T_CHECK)
  layer(2)
  for row = 0, SHEETC - 1 do
    for col = 0, SHEETC - 1 do
      local n = (top[kind()] + row) * SHEETC + col
      local t = n
      if tab ~= "tile" then
        t = W_SHEET + row * SHEETC + col
        tiledef(t, sprdef(n))
      end
      tile(192 // S + col, 16 // S + row, t)
    end
  end
  draw_selection()
end

local function draw_colours()
  tiledef(T_A, A == 0 and checker() or solid(A))
  tiledef(T_B, B == 0 and checker() or solid(B))
  layer(1)
  tile(0, 144 // S, T_A)
  tile(64 // S, 144 // S, T_B)
  say(16, 144, string.format("%03d", A), 3)
  say(80, 144, string.format("%03d", B), 3)
  local r, g, b = palette(A)
  for k = 1, 3 do
    fill1(128 + (k - 1) * 4 * S, 144, 4 * S, S, (rgb_edit and chan == k) and T_SEL or T_PANEL)
  end
  say(128, 144, string.format("R%03dG%03dB%03d", r, g, b), 12)
end

-- The letters along the bottom, a row each 16 pixels down from 160: the tools, then what they do to the whole tile.
local BUTTONS = {{"P", "F", "L", "R", "B", "K"}, {"X", "Y", "T", "C", "V"}, {"W", "A", "S", "D"}, {"Z", "G", "E", "U"}}

local function draw_buttons()
  for i, t in ipairs(BUTTONS[1]) do
    fill1(128 + (i - 1) * 2 * S, 160, S, S, tool == t and T_SEL or T_PANEL)
  end
  fill1(128 + 2 * S, 208, S, S, grid and T_SEL or T_PANEL)
  for r, row in ipairs(BUTTONS) do
    say(128, 160 + (r - 1) * 16, table.concat(row, " "), 12)
  end
end

local function draw_flags()
  layer(1)
  if tab ~= "tile" then
    say(128, 96, "", 4)
    fill1(128, 112, 64, 32, T_PANEL)
    return
  end
  say(128, 96, "FLAG", 4)
  local f = settileflags(cur.tile)
  for k = 0, 7 do
    layer(1)
    tile((128 + (k % 4) * 16) // S, (112 + (k // 4) * 16) // S, (f >> k) & 1 == 1 and T_BOX1 or T_BOX0)
  end
end

-- The previews, right of the drawing: the tile or sprite at its size at 128, 16; with tiles, its group of four (4n to 4n + 3, two by two in the
-- order a big actor takes sprites: 4n, 4n + 1 above 4n + 2, 4n + 3) beside it, and the tile three by three below them, to see how they join.
local function group_x() return 128 + 2 * S end
local function nine_y() return S == 8 and 40 or 48 end

-- The tile of the group of four under the pixel x, y, if the group is there.
local function group_at(x, y)
  local x4 = group_x()
  if tab == "tile" and not keys_page and x >= x4 and x < x4 + 2 * S and y >= 16 and y < 16 + 2 * S then
    return cur.tile - cur.tile % 4 + ((y - 16) // S) * 2 + (x - x4) // S
  end
end

local function draw_preview()
  fill1(128, 16, 64, 80, T_PANEL)
  layer(2)
  tilefill(128 // S, 16 // S, 64 // S, 80 // S, T_BLANK)
  fill1(128, 16, S, S, T_CHECK)
  if tab == "tile" then
    local x4, y9, g = group_x(), nine_y(), cur.tile - cur.tile % 4
    fill1(x4, 16, 2 * S, 2 * S, T_CHECK)
    fill1(128, y9, 3 * S, 3 * S, T_CHECK)
    layer(2)
    tile(128 // S, 16 // S, cur.tile)
    for k = 0, 3 do tile(x4 // S + k % 2, 16 // S + k // 2, g + k) end
    tilefill(128 // S, y9 // S, 3, 3, cur.tile)
    actorhide(actor_id, true)
  else
    actorhide(actor_id, false)
    actorpos(actor_id, 128, 16)
    if tab == "anim" then
      actoranim(actor_id, anim_n, true)
    else
      actoranim(actor_id, 0)
      actortile(actor_id, cur.spr)
    end
  end
end

local MODES = {"loop", "once", "pingpong"}

local function draw_anim()
  local mode, ticks, fr = animdef(anim_n)
  if anim_sel > #fr then anim_sel = math.max(#fr, 1) end
  local per = 320 // S
  fill1(0, 160, 320, 32, T_PANEL)
  for n = 2, 3 do
    layer(n)
    tilefill(0, 160 // S, per, 32 // S, T_BLANK)
  end
  for i = 1, 32 do
    local x, y = ((i - 1) % per) * S, 160 + ((i - 1) // per) * S
    if fr[i] then
      tiledef(W_FRAMES + i - 1, sprdef(fr[i][1]))
      fill1(x, y, S, S, T_CHECK)
      layer(2)
      tile(x // S, y // S, W_FRAMES + i - 1)
      if i == anim_sel then
        layer(3)
        tile(x // S, y // S, T_FRAME)
      end
    end
  end
  say(0, 144, string.format("<T%02d> ", anim_n) .. mode:upper(), 20)
  local f = fr[anim_sel]
  say(0, 192, f and string.format("F%02d S%03d T%03d", anim_sel, f[1], f[2]) or "NO FRAMES", 12)
  say(14 * S, 192, "+ -", 3)
  say(0, 208, "+ADD -DEL [ ]TIME M", 20)
  if tab == "anim" then actoranim(actor_id, anim_n, true) end
end

local function draw_bottom()
  layer(1)
  fill1(0, 144, 320, 80, T_PANEL)
  layer(2)
  tilefill(0, 144 // S, 320 // S, 80 // S, T_BLANK)
  layer(3)
  tilefill(0, 144 // S, 320 // S, 80 // S, T_BLANK)
  if tab == "anim" then
    draw_anim()
    return
  end
  local cw = 128 // S
  layer(1)
  for cy = 0, 64 // S - 1 do
    for cx = 0, cw - 1 do tile(cx, 160 // S + cy, W_PAL + cy * cw + cx) end
  end
  draw_colours()
  draw_buttons()
end

local function draw_all()
  layer(1)
  tilefill(0, 0, 320 // S, 240 // S, T_PANEL)
  for cy = 0, CELLS - 1 do
    for cx = 0, CELLS - 1 do tile(cx, 16 // S + cy, W_CANVAS + cy * CELLS + cx) end
  end
  layer(2)
  tilefill(0, 0, 320 // S, 240 // S, T_BLANK)
  layer(3)
  tilefill(0, 0, 320 // S, 240 // S, T_BLANK)
  load_pix()
  draw_canvas()
  draw_tabs()
  draw_sheet()
  draw_preview()
  draw_flags()
  draw_bottom()
  shown_status = nil
end

-- Where each line of KEYS goes, for the tile size.
local function place_keys()
  local rows, cols = (224 - 16) // S, (320 // S) // 20     -- 26 rows and two columns at 8 pixels, 13 and one at 16
  key_places = {}
  local col, row, i = 0, 0, 1
  while i <= #KEYS do
    local n = 1                                             -- the heading's lines
    while KEYS[i + n] and KEYS[i + n]:sub(1, 1) ~= "=" do n = n + 1 end
    if row > 0 and row + n > rows then col, row = col + 1, 0 end
    for j = i, i + n - 1 do
      key_places[j] = {col // cols + 1, (col % cols) * 20 * S, 16 + row * S}
      row = row + 1
    end
    i = i + n
  end
  key_pages = col // cols + 1
end

-- The keys tab: its page, in place of everything below the tabs.
local function draw_keys()
  layer(1)
  tilefill(0, 1, 320 // S, 240 // S - 1, T_PANEL)
  for n = 2, 3 do
    layer(n)
    tilefill(0, 1, 320 // S, 240 // S - 1, T_BLANK)
  end
  actorhide(actor_id, true)
  for j, line in ipairs(KEYS) do
    local p = key_places[j]
    if p[1] == keys_page then
      if line:sub(1, 1) == "=" then
        fill1(p[2], p[3], 19 * S, S, T_SEL)
        line = line:sub(2)
      end
      say(p[2], p[3], line)
    end
  end
  shown_status = nil
end

local function keys_turn(d)
  local p = (keys_page - 1 + d) % key_pages + 1
  if p ~= keys_page then
    keys_page = p
    draw_keys()
  end
end

-- Something about the item changed: what shows it besides the drawing.
local function shown_changed()
  if tab ~= "tile" then
    sheet_copy(cur.spr)
    if tab == "anim" then draw_anim() end
  end
  draw_tabs()
end

local function commit()
  local s = string.char(table.unpack(pix))
  if tab == "tile" then tiledef(cur.tile, s) else sprdef(cur.spr, s) end
  dirty = true
  shown_changed()
end

-- ---- undo ----

local function snapshot()
  return {k = kind(), n = item(), s = item_string()}
end

local function remember(step)
  undo[#undo + 1] = step
  if #undo > 100 then table.remove(undo, 1) end
  redo = {}
  dirty = true
end

-- Puts a step back; returns the step that undoes that.
local function swap(step)
  if step.k == "tile" then
    local now = tiledef(step.n)
    tiledef(step.n, step.s)
    return {k = "tile", n = step.n, s = now}
  elseif step.k == "spr" then
    local now = sprdef(step.n)
    sprdef(step.n, step.s)
    return {k = "spr", n = step.n, s = now}
  elseif step.k == "pal" then
    local r, g, b = palette(step.i)
    palette(step.i, step.r, step.g, step.b)
    return {k = "pal", i = step.i, r = r, g = g, b = b}
  elseif step.k == "flag" then
    local f = settileflags(step.n)
    settileflags(step.n, step.f)
    return {k = "flag", n = step.n, f = f}
  else
    local m, t, fr = animdef(step.n)
    animdef(step.n, step.m, step.t, step.fr)
    return {k = "anim", n = step.n, m = m, t = t, fr = fr}
  end
end

local function undo_redo(from, to, what)
  local step = table.remove(from)
  if not step then
    status("NOTHING TO " .. what)
    return
  end
  to[#to + 1] = swap(step)
  dirty = true
  draw_all()
end

-- ---- drawing tools ----

local function set_px(x, y, c)
  if x < 0 or y < 0 or x >= S or y >= S then return end
  local i = y * S + x + 1
  if pix[i] ~= c then
    pix[i] = c
    draw_pixel(x, y)
  end
end

local function line(x0, y0, x1, y1, c, put)
  local n = math.max(math.abs(x1 - x0), math.abs(y1 - y0))
  for i = 0, n do
    local x = n > 0 and x0 + math.floor((x1 - x0) * i / n + 0.5) or x0
    local y = n > 0 and y0 + math.floor((y1 - y0) * i / n + 0.5) or y0
    put(x, y, c)
  end
end

local function rect(x0, y0, x1, y1, c, filled, put)
  if x0 > x1 then x0, x1 = x1, x0 end
  if y0 > y1 then y0, y1 = y1, y0 end
  for y = y0, y1 do
    for x = x0, x1 do
      if filled or x == x0 or x == x1 or y == y0 or y == y1 then put(x, y, c) end
    end
  end
end

local function shape(x0, y0, x1, y1, c, put)
  if tool == "L" then line(x0, y0, x1, y1, c, put) else rect(x0, y0, x1, y1, c, tool == "B", put) end
end

local function flood(x, y, c)
  local from = pix[y * S + x + 1]
  if from == c then return end
  local stack = {x, y}
  while #stack > 0 do
    local py = table.remove(stack)
    local px = table.remove(stack)
    local i = py * S + px + 1
    if px >= 0 and py >= 0 and px < S and py < S and pix[i] == from then
      pix[i] = c
      stack[#stack + 1] = px + 1; stack[#stack + 1] = py
      stack[#stack + 1] = px - 1; stack[#stack + 1] = py
      stack[#stack + 1] = px; stack[#stack + 1] = py + 1
      stack[#stack + 1] = px; stack[#stack + 1] = py - 1
    end
  end
end

local function put_over(x, y, c)
  if x >= 0 and y >= 0 and x < S and y < S then over[y * S + x + 1] = c end
end

local function put_pix(x, y, c)
  if x >= 0 and y >= 0 and x < S and y < S then pix[y * S + x + 1] = c end
end

-- The pixel of the drawing under the pointer (kept inside the drawing).
local function canvas_xy()
  local x, y = mx // Z, (my - 16) // Z
  return math.max(0, math.min(S - 1, x)), math.max(0, math.min(S - 1, y))
end

local function in_canvas(x, y) return x >= 0 and x < 128 and y >= 16 and y < 144 end

local function stroke_end()
  if not stroke then return end
  if tool == "P" then
    commit()
    remember(stroke.step)
  else
    local x, y = canvas_xy()
    over = {}
    shape(stroke.ax, stroke.ay, x, y, stroke.c, put_pix)
    draw_canvas()
    commit()
    remember(stroke.step)
  end
  stroke = nil
end

local function stroke_move()
  if not stroke then return end
  local x, y = canvas_xy()
  if tool == "P" then
    if x ~= stroke.x or y ~= stroke.y then
      line(stroke.x, stroke.y, x, y, stroke.c, set_px)
      stroke.x, stroke.y = x, y
      commit()
    end
  elseif x ~= stroke.x or y ~= stroke.y then
    stroke.x, stroke.y = x, y
    over = {}
    shape(stroke.ax, stroke.ay, x, y, stroke.c, put_over)
    draw_canvas()
  end
end

-- A button went down on the drawing (from the keys: `keys` is true, and it comes up again at once).
local function canvas_press(btn, keys)
  stroke_end()                         -- (the other button's, if it is still down: it is one step to undo, this another)
  local x, y = canvas_xy()
  local c = btn == 1 and A or B
  if tool == "K" then
    if btn == 1 then A = pix[y * S + x + 1] else B = pix[y * S + x + 1] end
    draw_colours()
  elseif tool == "F" then
    local step = snapshot()
    flood(x, y, c)
    draw_canvas()
    commit()
    remember(step)
  elseif tool == "P" then
    stroke = {btn = btn, c = c, step = snapshot(), x = x, y = y}
    set_px(x, y, c)
    commit()
    if keys then stroke_end() end
  elseif keys then
    if pending then
      stroke = {btn = btn, c = pending.c, step = snapshot(), ax = pending.x, ay = pending.y, x = x, y = y}
      pending = nil
      stroke_end()
    else
      pending = {x = x, y = y, c = c}
      over = {}
      put_over(x, y, c)
      draw_canvas()
      status("SPACE AT OTHER END")
    end
  else
    stroke = {btn = btn, c = c, step = snapshot(), ax = x, ay = y, x = x, y = y}
    over = {}
    shape(x, y, x, y, c, put_over)
    draw_canvas()
  end
end

-- ---- what the whole item does ----

local function transform(f)
  local step = snapshot()
  local old = {table.unpack(pix)}
  for y = 0, S - 1 do
    for x = 0, S - 1 do
      local sx, sy = f(x, y)
      pix[y * S + x + 1] = old[sy * S + sx + 1]
    end
  end
  draw_canvas()
  commit()
  remember(step)
end

local function replace(s)
  local step = snapshot()
  for i = 1, S * S do pix[i] = s:byte(i) end
  draw_canvas()
  commit()
  remember(step)
end

local function select_item(n)
  n = n % 256
  cur[kind()] = n
  local row = n // SHEETC
  if row < top[kind()] or row >= top[kind()] + SHEETC then
    top[kind()] = math.min(row, 256 // SHEETC - SHEETC)
    draw_sheet()
  else
    draw_selection()
  end
  load_pix()
  pending, stroke = nil, nil
  draw_canvas()
  draw_tabs()
  draw_flags()
  draw_preview()
end

local function scroll_sheet(d)
  local t = math.max(0, math.min(256 // SHEETC - SHEETC, top[kind()] + d))
  if t ~= top[kind()] then
    top[kind()] = t
    draw_sheet()
  end
end

local rgb_close   -- (below)

-- Whatever is going on ends: a stroke, a line from the keys, the colour editor (a step to undo, if it changed the colour).
local function stop_doing()
  stroke_end()
  if pending then
    pending, over = nil, {}
    draw_canvas()
  end
  if rgb_edit then rgb_close() end
end

local function set_tab(t)
  if t == tab and not keys_page then return end
  stop_doing()
  local all = keys_page or tab == "tile" or t == "tile"
  tab, keys_page = t, nil
  if all then
    draw_all()
  else
    draw_tabs()
    draw_preview()
    draw_bottom()
  end
end

local function show_keys()
  stop_doing()
  keys_page = 1
  draw_tabs()
  draw_keys()
end

local function choose_colour(btn, c)
  if rgb_edit then rgb_close() end
  c = c % 256
  if btn == 1 then A = c else B = c end
  draw_colours()
end

local function toggle_flag(k)
  if tab ~= "tile" then return end
  local f = settileflags(cur.tile)
  remember({k = "flag", n = cur.tile, f = f})
  settileflags(cur.tile, f ~ (1 << k))
  draw_flags()
  draw_tabs()
end

-- ---- colour A's red, green and blue ----

local function rgb_change(d)
  local v = {palette(A)}
  v[chan] = math.max(0, math.min(255, v[chan] + d))
  palette(A, v[1], v[2], v[3])
  dirty = true
  draw_colours()
  draw_tabs()
end

local function rgb_open()
  if tab == "anim" then return end
  rgb_edit = true
  rgb_before = {palette(A)}
  rgb_before.i = A
  chan = 1
  draw_colours()
  status("ARROWS, ENTER: DONE", 4)
end

rgb_close = function()
  rgb_edit = false
  local r, g, b = palette(rgb_before.i)
  if r ~= rgb_before[1] or g ~= rgb_before[2] or b ~= rgb_before[3] then
    remember({k = "pal", i = rgb_before.i, r = rgb_before[1], g = rgb_before[2], b = rgb_before[3]})
  end
  draw_colours()
end

-- ---- animation tables ----

-- f(mode, frames) changes the frames (a copy) and returns the new mode, if it changes it.
local function anim_change(f)
  local mode, ticks, fr = animdef(anim_n)
  local _, _, before = animdef(anim_n)
  local m2 = f(mode, fr)
  remember({k = "anim", n = anim_n, m = mode, t = ticks, fr = before})
  animdef(anim_n, m2 or mode, ticks, fr)
  draw_anim()
  draw_tabs()
end

local function anim_add()
  local _, _, fr = animdef(anim_n)
  if #fr >= 32 then
    status("32 FRAMES AT MOST")
    return
  end
  anim_change(function(mode, fr)
    table.insert(fr, math.min(anim_sel + 1, #fr + 1), {cur.spr, 8})
    anim_sel = math.min(anim_sel + 1, #fr)
    if #fr == 1 then anim_sel = 1 end
  end)
end

local function anim_del()
  anim_change(function(mode, fr)
    if fr[anim_sel] then table.remove(fr, anim_sel) end
  end)
end

local function anim_ticks(d)
  anim_change(function(mode, fr)
    local f = fr[anim_sel]
    if f then f[2] = math.max(1, math.min(255, f[2] + d)) end
  end)
end

local function anim_mode()
  anim_change(function(mode, fr)
    for i, m in ipairs(MODES) do
      if m == mode then return MODES[i % 3 + 1] end
    end
  end)
end

local function anim_table(d)
  anim_n = (anim_n - 1 + d) % 32 + 1
  anim_sel = 1
  draw_anim()
end

local function anim_pick(i)
  local _, _, fr = animdef(anim_n)
  if fr[i] then
    anim_sel = i
    draw_anim()
    select_item(fr[i][1])
  end
end

-- ---- the file ----

local function save()
  local ok, err = pcall(savegfx, file)
  if ok then
    dirty = false
    status("SAVED " .. file)
    draw_tabs()
  else
    status("NOT SAVED: SEE TEXT", 4)
    print(err)
  end
  return ok
end

local start   -- (below)

local function open_other()
  local name, size, exists = ask_file(file)
  if name then
    cur, top, undo, redo, clip = {tile = 1, spr = 0}, {tile = 0, spr = 0}, {}, {}, nil
    start(name, size, exists)
  end
end

-- ---- the keys ----

local function move_pointer(dx, dy)
  local step = in_canvas(mx, my) and Z or S
  mouse(mx + dx * step, my + dy * step)
  mx, my = mouse()
end

local function set_back(d)
  back = (back - 1 + d) % #BACKS + 1
  tiledef(T_CHECK, checker())
  draw_canvas()
  if tab ~= "anim" then draw_colours() end   -- (a see-through A or B; the animation tables have the row instead)
  status("BACK: " .. BACKS[back][1])
end

local ACTIONS = {
  X = function() transform(function(x, y) return S - 1 - x, y end) end,
  Y = function() transform(function(x, y) return x, S - 1 - y end) end,
  T = function() transform(function(x, y) return y, S - 1 - x end) end,
  W = function() transform(function(x, y) return x, (y + 1) % S end) end,
  S = function() transform(function(x, y) return x, (y - 1) % S end) end,
  A = function() transform(function(x, y) return (x + 1) % S, y end) end,
  D = function() transform(function(x, y) return (x - 1) % S, y end) end,
  C = function() clip = item_string() status("COPIED") end,
  V = function() if clip then replace(clip) else status("NOTHING COPIED") end end,
  Z = function() undo_redo(undo, redo, "UNDO") end,
  G = function() grid = not grid draw_canvas() draw_buttons() end,
  E = function() if rgb_edit then rgb_close() else rgb_open() end end,
  U = function() set_back(1) end,
}

local function on_key(k, kclick)
  if ask then
    if k == "y" or k == "Y" then
      local what = ask
      ask = nil
      if save() then
        if what == "quit" then quit = true else open_other() end
      end
    elseif k == "n" or k == "N" then
      local what = ask
      ask = nil
      if what == "quit" then quit = true else open_other() end
    elseif k == "escape" then
      ask = nil
      status("")
    end
    return
  end
  if keys_page then                                    -- the keys tab: its pages, and the way out; the rest does nothing here
    if k == "f1" then set_tab("tile")
    elseif k == "f2" then set_tab("spr")
    elseif k == "f3" then set_tab("anim")
    elseif k == "f4" or k == "?" or k == "escape" or k == "tab" then set_tab(tab)
    elseif k == "pageup" or k == "up" or k == "left" or k == "," then keys_turn(-1)
    elseif k == "pagedown" or k == "down" or k == "right" or k == "." or k == " " then keys_turn(1)
    elseif k == "ctrl+s" then save()
    end
    return
  end
  if k == "f4" or k == "?" then
    show_keys()
    return
  end
  if rgb_edit then
    if k == "up" then rgb_change(1) return
    elseif k == "down" then rgb_change(-1) return
    elseif k == "pageup" then rgb_change(16) return
    elseif k == "pagedown" then rgb_change(-16) return
    elseif k == "left" then chan = (chan + 1) % 3 + 1 draw_colours() return
    elseif k == "right" then chan = chan % 3 + 1 draw_colours() return
    elseif k == "enter" or k == "escape" or k == "e" or k == "E" then rgb_close() return
    end
  end
  if k == "up" then move_pointer(0, -1)
  elseif k == "down" then move_pointer(0, 1)
  elseif k == "left" then move_pointer(-1, 0)
  elseif k == "right" then move_pointer(1, 0)
  elseif k == " " then if kclick then kclick(1) end
  elseif k == "enter" then if kclick then kclick(2) end
  elseif k == "escape" then
    if dirty then
      ask = "quit"
      status("SAVE FIRST? Y N ESC", 3600)
    else
      quit = true
    end
  elseif k == "ctrl+s" then save()
  elseif k == "ctrl+o" then
    if dirty then
      ask = "open"
      status("SAVE FIRST? Y N ESC", 3600)
    else
      open_other()
    end
  elseif k == "ctrl+z" then undo_redo(undo, redo, "UNDO")
  elseif k == "ctrl+y" or k == "Z" then undo_redo(redo, undo, "REDO")
  elseif k == "f1" then set_tab("tile")
  elseif k == "f2" then set_tab("spr")
  elseif k == "f3" then set_tab("anim")
  elseif k == "tab" then set_tab(tab == "tile" and "spr" or tab == "spr" and "anim" or "tile")
  elseif k == "delete" then replace(string.rep("\0", S * S))
  elseif k == "U" then set_back(-1)
  elseif k == "," or k == "pageup" then select_item(item() - 1)
  elseif k == "." or k == "pagedown" then select_item(item() + 1)
  elseif tab == "anim" and k == "<" then anim_table(-1)
  elseif tab == "anim" and k == ">" then anim_table(1)
  elseif tab == "anim" and (k == "m" or k == "M") then anim_mode()
  elseif tab == "anim" and k == "+" then anim_add()
  elseif tab == "anim" and (k == "-" or k == "backspace") then anim_del()
  elseif tab == "anim" and k == "[" then anim_ticks(-1)
  elseif tab == "anim" and k == "]" then anim_ticks(1)
  elseif k == "[" then choose_colour(1, A - 1)
  elseif k == "]" then choose_colour(1, A + 1)
  elseif #k == 1 and k >= "1" and k <= "8" then toggle_flag(k:byte() - 49)
  elseif #k == 1 then
    local u = k:upper()
    if u == "P" or u == "F" or u == "L" or u == "R" or u == "B" or u == "K" then
      tool = u
      pending = nil
      over = {}
      draw_canvas()
      draw_buttons()
    elseif ACTIONS[u] and k == u:lower() then
      ACTIONS[u]()
    end
  end
end

-- ---- the mouse ----

-- A button went down (from the mouse, or from the keys) where the pointer is.
local function press(btn, keys)
  local x, y = mx, my
  if y < S then                                                      -- the tabs
    local i = x // (4 * S) + 1
    if i <= 4 and x % (4 * S) < 3 * S then
      if i <= 3 then set_tab(({"tile", "spr", "anim"})[i])
      elseif keys_page then set_tab(tab)                             -- (the keys tab clicked again: back)
      else show_keys() end
    end
  elseif keys_page then                                              -- a page of the keys: the next one, the one before
    keys_turn(btn == 1 and 1 or -1)
  elseif in_canvas(x, y) then
    canvas_press(btn, keys)
  elseif x >= 192 and y >= 16 and y < 144 then                       -- the sheet
    local n = (top[kind()] + (y - 16) // S) * SHEETC + (x - 192) // S
    select_item(n)
  elseif group_at(x, y) then                                         -- one of the group of four
    select_item(group_at(x, y))
  elseif tab == "tile" and x >= 128 and x < 192 and y >= 112 and y < 144 then   -- the flags
    toggle_flag(((y - 112) // 16) * 4 + (x - 128) // 16)
  elseif tab == "anim" then
    if y >= 160 and y < 192 then
      anim_pick((y - 160) // S * (320 // S) + x // S + 1)
    elseif y >= 144 and y < 144 + S then
      local c = x // S
      if c == 0 then anim_table(-1) elseif c == 4 then anim_table(1) elseif c >= 6 then anim_mode() end
    elseif y >= 192 and y < 192 + S then
      local c = x // S
      if c == 14 then anim_add() elseif c == 16 then anim_del() end
    end
  elseif x < 128 and y >= 160 and y < 224 then                       -- the palette
    choose_colour(btn, ((y - 160) // 4) * 16 + x // 8)
  elseif x >= 128 and y >= 144 and y < 144 + S and rgb_edit then    -- red, green or blue
    chan = math.min(3, (x - 128) // (4 * S) + 1)
    draw_colours()
  elseif x >= 128 and y >= 160 and y < 224 then                      -- the buttons
    local c, row = (x - 128) // S, (y - 160) // 16 + 1
    local k = c % 2 == 0 and BUTTONS[row][c // 2 + 1]
    if k then on_key((row == 1 or (k == "U" and btn == 2)) and k or k:lower()) end   -- (U with the right button: back)
  end
end

local function wheel_turned(w)
  if keys_page then
    keys_turn(-w)
  elseif mx >= 192 and my >= 16 and my < 144 then
    scroll_sheet(-w)
  elseif tab == "anim" and my >= 160 and my < 192 then
    anim_ticks(w)
  elseif tab ~= "anim" and mx < 128 and my >= 160 and my < 224 then
    choose_colour(1, A + w)
  elseif rgb_edit then
    rgb_change(w)
  end
end

-- What is under the pointer, for the status line.
local function hover()
  local x, y = mx, my
  if keys_page then
    return key_pages > 1 and string.format("PAGE %d/%d PGUP PGDN", keys_page, key_pages) or "F4 OR ESC: BACK"
  elseif in_canvas(x, y) then
    local px, py = canvas_xy()
    return string.format("X%02d Y%02d C%03d", px, py, pix[py * S + px + 1])
  elseif x >= 192 and y >= 16 and y < 144 then
    return string.format("%s %03d", tab == "tile" and "TILE" or "SPRITE", (top[kind()] + (y - 16) // S) * SHEETC + (x - 192) // S)
  elseif group_at(x, y) then
    return string.format("TILE %03d OF %03d-%03d", group_at(x, y), cur.tile - cur.tile % 4, cur.tile - cur.tile % 4 + 3)
  elseif tab ~= "anim" and x < 128 and y >= 160 and y < 224 then
    local c = ((y - 160) // 4) * 16 + x // 8
    local r, g, b = palette(c)
    return string.format("C%03d R%03dG%03dB%03d", c, r, g, b)
  elseif tab == "tile" and x >= 128 and x < 192 and y >= 112 and y < 144 then
    local k = ((y - 112) // 16) * 4 + (x - 128) // 16
    return ({"FLAG 1 SOLID", "FLAG 2 ONE-WAY", "FLAG 4 TRIGGER", "FLAG 8 GAME", "FLAG 16 GAME", "FLAG 32 GAME", "FLAG 64 GAME", "FLAG 128 GAME"})[k + 1]
  end
  return file .. (dirty and " *" or "")
end

local function draw_status()
  local s = millis() < msg_until and msg or hover()
  if s ~= shown_status then
    shown_status = s
    say(0, 224, s, 320 // S)
  end
end

-- ---- starting ----

start = function(name, size, exists, font_tiles)
  file = name
  gfx(size, plan(size, font_tiles or 0))
  keys_page = nil
  place_keys()
  palette("\0", -1, -1, -1)                 -- (every other character is its own colour in the strings this uses)
  if exists then
    local ok, err = pcall(loadgfx, name)
    if not ok then
      print(err)
      quit = true
      return
    end
  end
  dirty = false
  -- the letters for the editor's text: in the file's empty tiles, or in working tiles if it has not enough
  local ok, added = pcall(font, CHARS, WHITE, 1, 255)
  if not ok and not font_tiles then
    return start(name, size, exists, 64)       -- again, with working tiles for the letters (only then: they take 16 KB with tiles of 16)
  end
  if not ok then
    local ok2, err = pcall(font, CHARS, WHITE, W_FONT, W_FONT + 63)
    if not ok2 then
      print(err)
      quit = true
      return
    end
    status("FONT IN WORK TILES", 4)
  elseif added > 0 then
    dirty = true
    status(added == 1 and "1 TILE FOR A LETTER" or string.format("%d TILES FOR LETTERS", added), 4)
  end
  tiledef(T_BLANK, solid(0))
  tiledef(T_PANEL, solid(BLACK))
  tiledef(T_SEL, solid(BLUE))
  tiledef(T_POINTER, arrow())
  tiledef(T_FRAME, box(YELLOW, 0))
  tiledef(T_BOX0, box(GREY, 0))
  tiledef(T_BOX1, box(WHITE, YELLOW))
  tiledef(T_CHECK, checker())
  local cw = 128 // S
  for cy = 0, 64 // S - 1 do                 -- the palette: 16 x 16 swatches of 8 x 4 pixels
    for cx = 0, cw - 1 do
      local t = {}
      for py = 0, S - 1 do
        for px = 0, S - 1 do
          t[#t + 1] = ch(((cy * S + py) // 4) * 16 + (cx * S + px) // 8)
        end
      end
      tiledef(W_PAL + cy * cw + cx, table.concat(t))
    end
  end
  for n = 1, 3 do
    newmap(320 // S, 240 // S, n)
  end
  newmap(1, 1, 4)
  layer(4)
  tile(0, 0, T_POINTER)
  actor_id = actor(0, 128, 16)
  mouse(mx, my)
  shown_status = nil
  draw_all()
end

-- ---- the program ----

local name, size, exists = ask_file("DRAW.TGF")
if not name then return end
start(name, size, exists)
while not quit do
  waitframe()
  local x, y, left, right, middle, wheel = mouse()
  if x ~= mx or y ~= my then
    mx, my = x, y
    stroke_move()
  end
  parallax(4, 0, 0, -mx, -my)
  local keyclick
  local k = key()
  while k and not quit do
    local before = keyclick
    on_key(k, function(b) keyclick = b end)
    if keyclick and not before then
      press(keyclick, true)
      keyclick = nil
    end
    k = key()
  end
  if quit then break end
  for b = 1, 2 do
    if click(b == 1 and "left" or "right") and not ask then press(b, false) end
    if stroke and stroke.btn == b and not (b == 1 and left or b == 2 and right) then stroke_end() end
  end
  if click("middle") and in_canvas(mx, my) and not keys_page then
    local px, py = canvas_xy()
    A = pix[py * S + px + 1]
    draw_colours()
  end
  if wheel ~= 0 then wheel_turned(wheel) end
  draw_status()
end
print("Left the editor" .. (dirty and " without saving." or "."))
