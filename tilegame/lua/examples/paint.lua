-- Painting with the mouse and the keys (mouse, click, key). The left button paints with the colour chosen and the right one picks up the colour
-- under the pointer. A colour along the top, the keys 1 to 8 or the wheel choose one. Without a mouse the arrow keys move the pointer and space
-- paints. c clears the picture, s saves it to the card (paint.map) and l loads it back. Escape stops the program.

gfx(8)
local rgb = {{0, 0, 0}, {255, 255, 255}, {220, 40, 40}, {40, 170, 60}, {50, 90, 220}, {240, 210, 40}, {230, 120, 30}, {150, 60, 190}}
for i = 1, 8 do
  palette(tostring(i), rgb[i][1], rgb[i][2], rgb[i][3])
  tiledef(i, string.rep(tostring(i), 64))   -- tile n is all colour n
end
palette("9", 110, 110, 110)
tiledef(9, string.rep("9", 64))             -- the bar along the top
sprdef(1, "1......." .. "11......" .. "121....." .. "1221...." .. "12221..." .. "1211...." .. "11.11..." .. "....1...")   -- the pointer
newmap(40, 30)                              -- the whole screen: 320 x 240 pixels

local colour = 1
local flash, flash_tile = 0, 9              -- the bar turns green for a moment after saving or loading, red if loading failed

local function bar()
  for i = 1, 8 do
    tilefill((i - 1) * 3, 0, 3, 2, i)
  end
  tilefill(24, 0, 12, 2, flash > 0 and flash_tile or 9)
  tilefill(36, 0, 4, 2, colour)             -- the colour chosen
end

local function clear()
  tilefill(0, 2, 40, 28, 2)
end

local function choose(c)
  colour = c
  bar()
end

local function paint_line(x0, y0, x1, y1)   -- every cell from one to the other, so that a quick stroke has no gaps
  local n = math.max(math.abs(x1 - x0), math.abs(y1 - y0))
  for i = 0, n do
    local x = n > 0 and x0 + math.floor((x1 - x0) * i / n + 0.5) or x0
    local y = n > 0 and y0 + math.floor((y1 - y0) * i / n + 0.5) or y0
    if y >= 2 then
      tile(x, y, colour)
    end
  end
end

bar()
clear()
local pointer = actor(1, 160, 120)
local lastx, lasty                          -- the cell painted at the last frame, while the button is held
local done = false
repeat
  waitframe()
  if flash > 0 then
    flash = flash - 1
    if flash == 0 then bar() end
  end

  local k = key()
  while k do
    local x, y = mouse()
    if #k == 1 and k:match("[1-8]") then
      choose(tonumber(k))
    elseif k == "up" then
      mouse(x, y - 8)
    elseif k == "down" then
      mouse(x, y + 8)
    elseif k == "left" then
      mouse(x - 8, y)
    elseif k == "right" then
      mouse(x + 8, y)
    elseif k == " " then
      paint_line(x // 8, y // 8, x // 8, y // 8)
    elseif k == "c" then
      clear()
    elseif k == "s" then
      savemap("paint.map")
      flash, flash_tile = 30, 4
      bar()
    elseif k == "l" then
      flash, flash_tile = 30, pcall(loadmap, "paint.map") and 4 or 3
      bar()
    elseif k == "escape" then
      done = true
    end
    k = key()
  end

  local x, y, left, right, middle, wheel = mouse()
  actorpos(pointer, x, y)
  local cx, cy = x // 8, y // 8
  if left and cy >= 2 then
    paint_line(lastx or cx, lasty or cy, cx, cy)
    lastx, lasty = cx, cy
  else
    lastx, lasty = nil, nil
  end
  if click() and cy < 2 and cx < 24 then
    choose(cx // 3 + 1)
  end
  if right then
    local t = gettile(cx, cy)
    if t >= 1 and t <= 8 then choose(t) end
  end
  if wheel ~= 0 then
    choose((colour - 1 - wheel) % 8 + 1)
  end
until done
print("Painted.")
