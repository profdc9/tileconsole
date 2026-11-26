-- Four tile layers: stars far away (a quarter of the camera's speed), hills nearer (half its speed across, and fixed at the bottom of the screen),
-- the room the ball walks in, and a panel at the top of the screen that stays put, in front of the ball. The arrow keys move the ball, B ends the program.

gfx(8)
palette(0, 10, 10, 40)              -- the backdrop: a dark blue sky
palette("1", 120, 80, 40)           -- wall
palette("3", 30, 80, 45)            -- hills
palette("4", 250, 210, 40)          -- the ball
palette("5", 255, 255, 220)         -- a star
palette("6", 70, 70, 110)           -- the panel
palette("7", 150, 150, 200)         -- its edge

tiledef(1, string.rep("1", 64))                                   -- a wall
tiledef(2, string.rep(".", 27) .. "5" .. string.rep(".", 36))     -- a star: one pixel
tiledef(3, string.rep("3", 64))                                   -- the inside of a hill
tiledef(4, "...33..." .. ".333333." .. string.rep("3", 48))       -- the top of a hill
tiledef(5, string.rep("6", 64))                                   -- the panel
tiledef(6, string.rep("6", 56) .. string.rep("7", 8))             -- the panel's lower edge

sprdef(1, "..4444.." ..                                           -- the ball
          ".444444." ..
          string.rep("4", 32) ..
          ".444444." ..
          "..4444..")

-- layer 1: stars, far away, repeating
newmap(64, 32, 1)
layer(1)
for i = 1, 150 do tile(i * 37 % 64, i * 23 % 32, 2) end
wrap(true)
parallax(1, 0.25)

-- layer 2: hills, nearer, repeating
newmap(64, 32, 2)
layer(2)
for x = 0, 63 do
  local h = 4 + math.floor(3 * math.sin(x / 4) + 2 * math.sin(x / 9))
  tilefill(x, 32 - h, 1, h, 3)
  tile(x, 32 - h, 4)
end
wrap(true)
parallax(2, 0.5, 0, 0, 16)          -- half speed across, not at all up and down: the hills stay at the bottom of the screen (256 - 240 = 16)

-- layer 3: the room, where the ball is; its floor is see-through, so the hills and the stars show
newmap(80, 60, 3)
layer(3)
tilefill(0, 0, 80, 1, 1)
tilefill(0, 59, 80, 1, 1)
tilefill(0, 0, 1, 60, 1)
tilefill(79, 0, 1, 60, 1)
for i = 1, 40 do tilefill(2 + i * 13 % 74, 3 + i * 7 % 54, 2, 2, 1) end
settileflags(1, 1)                  -- walls are solid
solidlayer(3)

-- layer 4: a panel at the top of the screen, which does not move
newmap(40, 2, 4)
layer(4)
tilefill(0, 0, 40, 1, 5)
tilefill(0, 1, 40, 1, 6)
parallax(4, 0, 0)

p = actor(1, 160, 120)
actorlayer(p, 3)                    -- in front of the room, behind the panel
follow(p)

function update(k)
  local dx, dy = 0, 0
  if btn(0, "left") then dx = -2 end
  if btn(0, "right") then dx = 2 end
  if btn(0, "up") then dy = -2 end
  if btn(0, "down") then dy = 2 end
  actormove(p, dx, dy)
  return btn(0, "b")
end

onframe(update)
run()

print(actorx(p), actory(p), camx(), camy())
