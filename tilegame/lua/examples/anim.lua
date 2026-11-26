-- A creature that walks around a room, with its feet animated by animation tables. The arrow keys move it, B ends the program.
-- animdef(n, mode, ticks, frames) makes table n out of sprites ("2 1 3 1" shows sprite 2, then 1, 3, 1; "2:10" would show sprite 2 for 10 frames
-- instead of ticks). actoranim(a, n) says which table the actor plays now: it can be said every frame, it only starts over when n changes.

gfx(8)
palette("1", 120, 80, 40)           -- wall
palette("2", 40, 120, 60)           -- floor
palette("4", 250, 210, 40)          -- the body
palette("5", 20, 20, 20)            -- the eyes and the feet

for y = 0, 7 do
  tilerow(1, y, "11111111")
  tilerow(2, y, "22222222")
end

sprdef(1, "..4444.." ..             -- sprite 1: standing
          ".444444." ..
          ".454454." ..
          ".444444." ..
          ".444444." ..
          "..4444.." ..
          "..5..5.." ..
          "..5..5..")
for n = 2, 3 do                     -- sprites 2 and 3: the same body, the feet apart and together
  for y = 0, 5 do
    sprrow(n, y, ".444444.")
  end
  sprrow(n, 2, ".454454.")
end
sprrow(2, 6, ".5....5.")
sprrow(2, 7, ".5....5.")
sprrow(3, 6, "...55...")
sprrow(3, 7, "...55...")

animdef(1, "loop", 1, "1")          -- standing: one sprite
animdef(2, "loop", 6, "2 1 3 1")    -- walking: a step every 6 frames

newmap(40, 30)
tilefill(0, 0, 40, 30, 1)
tilefill(1, 1, 38, 28, 2)
settileflags(1, 1)

p = actor(1, 160, 120)
actoranim(p, 1)

function update(k)
  local dx, dy = 0, 0
  if btn(0, "left") then dx = -1 end
  if btn(0, "right") then dx = 1 end
  if btn(0, "up") then dy = -1 end
  if btn(0, "down") then dy = 1 end
  actormove(p, dx, dy)
  if dx ~= 0 or dy ~= 0 then
    actoranim(p, 2)
  else
    actoranim(p, 1)
  end
  if dx < 0 then actorflip(p, 1) end
  if dx > 0 then actorflip(p, 0) end
  return btn(0, "b")
end

onframe(update)
run()
