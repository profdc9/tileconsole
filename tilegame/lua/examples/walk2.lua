-- Walk around a room, the same as walk.lua but with the frame loop in the system: run() calls update once a frame (k is the frames that went by),
-- and a result that is not nil or false (B pressed) ends it. follow(p) keeps the camera on the ball. The arrow keys move the ball.

gfx(8)
palette("1", 120, 80, 40)           -- wall
palette("2", 40, 120, 60)           -- floor
palette("3", 70, 150, 80)           -- a lighter spot on the floor
palette("4", 250, 210, 40)          -- the ball

for y = 0, 7 do
  tilerow(1, y, "11111111")         -- tile 1: wall
  tilerow(2, y, "22222222")         -- tile 2: floor
  tilerow(3, y, "22222222")         -- tile 3: floor with a spot
end
tilerow(3, 3, "22233222")
tilerow(3, 4, "22233222")

sprrow(1, 0, "..4444..")            -- sprite 1: the ball
sprrow(1, 1, ".444444.")
for y = 2, 5 do
  sprrow(1, y, "44444444")
end
sprrow(1, 6, ".444444.")
sprrow(1, 7, "..4444..")

newmap(80, 60)                      -- 640 x 480 pixels, bigger than the screen, so there is something to scroll
tilefill(0, 0, 80, 60, 1)
tilefill(1, 1, 78, 58, 2)
for i = 1, 120 do
  tile(1 + i * 7 % 78, 1 + i * 11 % 58, 3)
end
settileflags(1, 1)                  -- walls are solid

p = actor(1, 160, 120)

function update(k)
  local dx, dy = 0, 0
  if btn(0, "left") then dx = -2 end
  if btn(0, "right") then dx = 2 end
  if btn(0, "up") then dy = -2 end
  if btn(0, "down") then dy = 2 end
  actormove(p, dx, dy)
  return btn(0, "b")
end

follow(p)                           -- the camera follows the ball
onframe(update)
run()

print(actorx(p))
print(actory(p))
